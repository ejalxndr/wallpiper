/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xcb/dri3.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <cJSON.h>
#include <stb_image_write.h>

#include "wallpiper/capture_socket.h"
#include "wallpiper/ctl_protocol.h"
#include "wallpiper/debug_overlay.h"
#include "wallpiper/monitor_geometry.h"

/* must match loader/vk-layer-hook/config.h */
#define WP_I3_CAPTURE_SLOT_COUNT 3
#define WP_I3_MAX_CAPTURE_CHANNELS 4
#define WP_I3_MAX_OUTPUTS WP_I3_MAX_CAPTURE_CHANNELS

typedef struct {
  bool in_use;
  uint32_t slot;
  xcb_pixmap_t pixmap;
  uint32_t width;
  uint32_t height;
} wp_i3_slot_t;

typedef struct {
  bool valid;
  xcb_pixmap_t pixmap;
  xcb_shm_seg_t seg;
  uint32_t width;
  uint32_t height;
} wp_i3_shm_pixmap_t;

typedef enum {
  WP_I3_SOURCE_NONE,
  WP_I3_SOURCE_SLOT,
  WP_I3_SOURCE_SHM,
} wp_i3_source_kind_t;

typedef struct {
  bool known;
  char name[64];
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;

  xcb_window_t window;
  xcb_gcontext_t gc;

  bool has_bound_channel;
  uint32_t bound_channel;

  wp_i3_slot_t slots[WP_I3_CAPTURE_SLOT_COUNT];
  wp_i3_shm_pixmap_t current_shm_pixmap;
  wp_i3_source_kind_t current_source_kind;
  uint32_t current_source_slot;
} wp_i3_output_t;

typedef struct {
  xcb_connection_t *conn;
  xcb_window_t root;
  xcb_visualid_t visual;
  uint8_t depth;

  wp_monitor_geometry_t geometry;

  wp_i3_output_t outputs[WP_I3_MAX_OUTPUTS];
  size_t output_count;

  bool debug_enabled;
  wp_debug_throttle_t debug_throttle;
  wp_frame_stats_t *stats;
} wp_i3_state_t;

static char *run_command(const char *const argv[]) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return NULL;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return NULL;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }

  close(pipefd[1]);

  size_t cap = 65536;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    return NULL;
  }

  for (;;) {
    if (len + 4096 > cap) {
      cap *= 2;
      char *grown = realloc(buf, cap);
      if (!grown) {
        free(buf);
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return NULL;
      }
      buf = grown;
    }
    ssize_t n = read(pipefd[0], buf + len, 4096);
    if (n <= 0) {
      break;
    }
    len += (size_t)n;
  }
  buf[len] = '\0';
  close(pipefd[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    free(buf);
    return NULL;
  }

  return buf;
}

static bool try_detect_outputs(wp_i3_state_t *state) {
  const char *out_argv[] = {"i3-msg", "-t", "get_outputs", NULL};
  char *out_output = run_command(out_argv);
  if (!out_output) {
    return false;
  }

  cJSON *outputs = cJSON_Parse(out_output);
  free(out_output);
  if (!outputs || !cJSON_IsArray(outputs)) {
    cJSON_Delete(outputs);
    return false;
  }

  size_t count = 0;
  int out_count = cJSON_GetArraySize(outputs);
  for (int i = 0; i < out_count && count < WP_I3_MAX_OUTPUTS; i++) {
    cJSON *output = cJSON_GetArrayItem(outputs, i);
    cJSON *active = cJSON_GetObjectItem(output, "active");
    if (!active || !cJSON_IsTrue(active)) {
      continue;
    }
    cJSON *name = cJSON_GetObjectItem(output, "name");
    cJSON *rect = cJSON_GetObjectItem(output, "rect");
    cJSON *x = cJSON_GetObjectItem(rect, "x");
    cJSON *y = cJSON_GetObjectItem(rect, "y");
    cJSON *width = cJSON_GetObjectItem(rect, "width");
    cJSON *height = cJSON_GetObjectItem(rect, "height");
    if (!cJSON_IsString(name) || !cJSON_IsNumber(x) || !cJSON_IsNumber(y) ||
        !cJSON_IsNumber(width) || !cJSON_IsNumber(height)) {
      continue;
    }

    wp_i3_output_t *out = &state->outputs[count];
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name->valuestring);
    out->x = (int32_t)x->valuedouble;
    out->y = (int32_t)y->valuedouble;
    out->width = (uint32_t)width->valuedouble;
    out->height = (uint32_t)height->valuedouble;
    out->known = true;
    count++;
  }

  cJSON_Delete(outputs);

  if (count == 0) {
    return false;
  }
  state->output_count = count;
  return true;
}

static void detect_outputs(wp_i3_state_t *state) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (try_detect_outputs(state)) {
      return;
    }
    printf("output detection attempt %d failed, retrying\n", attempt);
    usleep(500 * 1000);
  }
  printf("output detection failed after retries, falling back to a single "
         "1920x1080 output at 0,0\n");
  wp_i3_output_t *out = &state->outputs[0];
  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "fallback");
  out->x = 0;
  out->y = 0;
  out->width = 1920;
  out->height = 1080;
  out->known = true;
  state->output_count = 1;
}

static void cursor_pos_fn(void *ctx, wp_ctl_response_t *out) {
  (void)ctx;
  memset(out, 0, sizeof(*out));

  xcb_connection_t *conn = xcb_connect(NULL, NULL);
  if (!conn || xcb_connection_has_error(conn)) {
    if (conn) {
      xcb_disconnect(conn);
    }
    out->tag = WP_CTL_RESPONSE_ERR;
    snprintf(out->err, sizeof(out->err), "%s", "cursor position unavailable");
    return;
  }

  const xcb_setup_t *setup = xcb_get_setup(conn);
  xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;

  xcb_query_pointer_cookie_t cookie = xcb_query_pointer(conn, screen->root);
  xcb_generic_error_t *err = NULL;
  xcb_query_pointer_reply_t *reply =
      xcb_query_pointer_reply(conn, cookie, &err);

  if (reply) {
    out->tag = WP_CTL_RESPONSE_CURSOR_POS;
    out->cursor_x = reply->root_x;
    out->cursor_y = reply->root_y;
    free(reply);
  } else {
    out->tag = WP_CTL_RESPONSE_ERR;
    snprintf(out->err, sizeof(out->err), "%s", "cursor position unavailable");
  }
  free(err);
  xcb_disconnect(conn);
}

static void refresh_buffer(wp_i3_state_t *state, wp_i3_output_t *out) {
  xcb_pixmap_t pixmap = XCB_NONE;
  uint32_t width = 0;
  uint32_t height = 0;

  if (out->current_source_kind == WP_I3_SOURCE_SLOT) {
    uint32_t local_idx = out->current_source_slot % WP_I3_CAPTURE_SLOT_COUNT;
    wp_i3_slot_t *slot = &out->slots[local_idx];
    if (!slot->in_use || slot->slot != out->current_source_slot) {
      return;
    }
    pixmap = slot->pixmap;
    width = slot->width;
    height = slot->height;
  } else if (out->current_source_kind == WP_I3_SOURCE_SHM) {
    if (!out->current_shm_pixmap.valid) {
      return;
    }
    pixmap = out->current_shm_pixmap.pixmap;
    width = out->current_shm_pixmap.width;
    height = out->current_shm_pixmap.height;
  } else {
    return;
  }

  xcb_copy_area(state->conn, pixmap, out->window, out->gc, 0, 0, 0, 0,
                (uint16_t)width, (uint16_t)height);
  xcb_flush(state->conn);

  wp_frame_stats_record_display(state->stats);

  if (state->debug_enabled && out == &state->outputs[0]) {
    static uint8_t pixels[WP_DEBUG_OVERLAY_BUFFER_SIZE];
    wp_render_stats_panel(state->stats, pixels);
    int32_t overlay_y = ((int32_t)out->height - WP_DEBUG_OVERLAY_HEIGHT) / 2;
    if (overlay_y < 0) {
      overlay_y = 0;
    }
    xcb_put_image(state->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, out->window, out->gc,
                  WP_DEBUG_OVERLAY_WIDTH, WP_DEBUG_OVERLAY_HEIGHT, 12,
                  (int16_t)overlay_y, 0, state->depth, sizeof(pixels), pixels);
    xcb_flush(state->conn);
  }
}

static void set_current_source(wp_i3_state_t *state, wp_i3_output_t *out,
                               wp_i3_source_kind_t kind, uint32_t slot) {
  out->current_source_kind = kind;
  out->current_source_slot = slot;
  refresh_buffer(state, out);
}

static void draw_debug_overlay(wp_i3_state_t *state) {
  wp_i3_output_t *out = &state->outputs[0];
  static uint8_t pixels[WP_DEBUG_OVERLAY_BUFFER_SIZE];
  wp_render_stats_panel(state->stats, pixels);
  int32_t overlay_y = ((int32_t)out->height - WP_DEBUG_OVERLAY_HEIGHT) / 2;
  if (overlay_y < 0) {
    overlay_y = 0;
  }
  xcb_put_image(state->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, out->window, out->gc,
                WP_DEBUG_OVERLAY_WIDTH, WP_DEBUG_OVERLAY_HEIGHT, 12,
                (int16_t)overlay_y, 0, state->depth, sizeof(pixels), pixels);
  xcb_flush(state->conn);
}

static void set_debug_enabled(wp_i3_state_t *state, bool enabled) {
  state->debug_enabled = enabled;
  wp_debug_throttle_reset(&state->debug_throttle);
  if (enabled) {
    draw_debug_overlay(state);
  } else {
    refresh_buffer(state, &state->outputs[0]);
  }
  printf("[ctl] debug overlay -> %s\n", enabled ? "true" : "false");
}

static void maybe_redraw_debug(wp_i3_state_t *state) {
  if (wp_debug_throttle_should_redraw(&state->debug_throttle)) {
    draw_debug_overlay(state);
  }
}

static wp_i3_output_t *find_output_for_channel(wp_i3_state_t *state,
                                               uint32_t channel) {
  for (size_t i = 0; i < state->output_count; i++) {
    wp_i3_output_t *out = &state->outputs[i];
    if (out->has_bound_channel && out->bound_channel == channel) {
      return out;
    }
  }
  return NULL;
}

static wp_i3_output_t *claim_output_for_size(wp_i3_state_t *state,
                                             uint32_t channel, uint32_t width,
                                             uint32_t height) {
  for (size_t i = 0; i < state->output_count; i++) {
    wp_i3_output_t *out = &state->outputs[i];
    if (out->known && !out->has_bound_channel && out->width == width &&
        out->height == height) {
      out->has_bound_channel = true;
      out->bound_channel = channel;
      return out;
    }
  }

  wp_i3_output_t *best = NULL;
  for (size_t i = 0; i < state->output_count; i++) {
    wp_i3_output_t *out = &state->outputs[i];
    if (!out->known || out->has_bound_channel) {
      continue;
    }
    if (!best || out->x < best->x) {
      best = out;
    }
  }
  if (best) {
    best->has_bound_channel = true;
    best->bound_channel = channel;
  }
  return best;
}

static void handle_buf(wp_i3_state_t *state, uint32_t wire_slot, uint32_t width,
                       uint32_t height, uint32_t format, uint32_t stride,
                       uint64_t modifier, int fd) {
  uint32_t channel = wire_slot / WP_I3_CAPTURE_SLOT_COUNT;
  uint32_t local_idx = wire_slot % WP_I3_CAPTURE_SLOT_COUNT;
  if (channel >= WP_I3_MAX_CAPTURE_CHANNELS) {
    printf("[socket] bad wire slot %u, dropping\n", wire_slot);
    close(fd);
    return;
  }

  wp_i3_output_t *out = find_output_for_channel(state, channel);
  if (!out) {
    out = claim_output_for_size(state, channel, width, height);
    if (!out) {
      printf("[socket] no output available for new channel %u (%ux%u), "
             "dropping\n",
             channel, width, height);
      close(fd);
      return;
    }
    printf("[socket] channel %u bound to output=%s at (%d,%d) for stream "
           "%ux%u\n",
           channel, out->name, out->x, out->y, width, height);
  }

  wp_i3_slot_t *slot = &out->slots[local_idx];

  xcb_pixmap_t pixmap = xcb_generate_id(state->conn);
  xcb_void_cookie_t cookie = xcb_dri3_pixmap_from_buffers_checked(
      state->conn, pixmap, out->window, 1, (uint16_t)width, (uint16_t)height,
      stride, 0, 0, 0, 0, 0, 0, 0, state->depth, 32, modifier, &fd);
  xcb_generic_error_t *err = xcb_request_check(state->conn, cookie);
  if (err) {
    printf("[socket] dri3 pixmap import failed for slot %u\n", wire_slot);
    free(err);
    return;
  }

  if (slot->in_use) {
    xcb_free_pixmap(state->conn, slot->pixmap);
  }

  slot->in_use = true;
  slot->slot = wire_slot;
  slot->pixmap = pixmap;
  slot->width = width;
  slot->height = height;

  printf("[socket] output=%s registered capture slot %u (channel=%u "
         "local=%u) %ux%u VkFormat=%u stride=%u modifier=%llu (dri3 pixmap "
         "import assumes the server's default 24/32-bit TrueColor visual "
         "layout, not this format)\n",
         out->name, wire_slot, channel, local_idx, width, height, format,
         stride, (unsigned long long)modifier);
  set_current_source(state, out, WP_I3_SOURCE_SLOT, wire_slot);
}

static void handle_shm(wp_i3_state_t *state, uint32_t width, uint32_t height,
                       uint32_t stride, int fd) {
  if (state->output_count == 0) {
    close(fd);
    return;
  }
  wp_i3_output_t *out = &state->outputs[0];

  if (stride != width * 4) {
    printf("[socket] shm frame stride %u doesn't match tightly-packed %ux4, "
           "X11's MIT-SHM CreatePixmap can't "
           "represent padded rows - dropping frame\n",
           stride, width);
    close(fd);
    return;
  }

  xcb_shm_seg_t seg = xcb_generate_id(state->conn);
  xcb_void_cookie_t attach_cookie =
      xcb_shm_attach_fd_checked(state->conn, seg, fd, 1);
  xcb_generic_error_t *attach_err =
      xcb_request_check(state->conn, attach_cookie);
  if (attach_err) {
    printf("[socket] shm attach_fd failed\n");
    free(attach_err);
    return;
  }

  xcb_pixmap_t pixmap = xcb_generate_id(state->conn);
  xcb_void_cookie_t cookie = xcb_shm_create_pixmap_checked(
      state->conn, pixmap, out->window, (uint16_t)width, (uint16_t)height,
      state->depth, seg, 0);
  xcb_generic_error_t *err = xcb_request_check(state->conn, cookie);
  if (err) {
    printf("[socket] shm pixmap import failed\n");
    free(err);
    xcb_shm_detach(state->conn, seg);
    return;
  }

  if (out->current_shm_pixmap.valid) {
    xcb_free_pixmap(state->conn, out->current_shm_pixmap.pixmap);
    xcb_shm_detach(state->conn, out->current_shm_pixmap.seg);
  }
  out->current_shm_pixmap.valid = true;
  out->current_shm_pixmap.pixmap = pixmap;
  out->current_shm_pixmap.seg = seg;
  out->current_shm_pixmap.width = width;
  out->current_shm_pixmap.height = height;

  set_current_source(state, out, WP_I3_SOURCE_SHM, 0);
}

static void handle_capture_event(wp_i3_state_t *state,
                                 const wp_capture_event_t *event) {
  wp_frame_stats_record_capture(state->stats);

  switch (event->tag) {
  case WP_CAPTURE_EVENT_BUF: {
    int image_fd = event->fds[0];
    if (event->nfds > 1) {
      close(event->fds[1]);
    }
    handle_buf(state, event->slot, event->width, event->height,
               event->format, event->stride, event->modifier, image_fd);
    break;
  }
  case WP_CAPTURE_EVENT_FRAME: {
    if (event->nfds > 0) {
      close(event->fds[0]);
    }
    uint32_t channel = event->slot / WP_I3_CAPTURE_SLOT_COUNT;
    uint32_t local_idx = event->slot % WP_I3_CAPTURE_SLOT_COUNT;
    wp_i3_output_t *out = find_output_for_channel(state, channel);
    if (out && out->slots[local_idx].in_use) {
      set_current_source(state, out, WP_I3_SOURCE_SLOT, event->slot);
    }
    break;
  }
  case WP_CAPTURE_EVENT_SHM: {
    handle_shm(state, event->width, event->height, event->stride,
               event->fds[0]);
    break;
  }
  }
}

static void detach(wp_i3_state_t *state) {
  for (size_t i = 0; i < state->output_count; i++) {
    wp_i3_output_t *out = &state->outputs[i];
    for (size_t j = 0; j < WP_I3_CAPTURE_SLOT_COUNT; j++) {
      if (out->slots[j].in_use) {
        xcb_free_pixmap(state->conn, out->slots[j].pixmap);
        out->slots[j].in_use = false;
      }
    }
    if (out->current_shm_pixmap.valid) {
      xcb_free_pixmap(state->conn, out->current_shm_pixmap.pixmap);
      xcb_shm_detach(state->conn, out->current_shm_pixmap.seg);
      out->current_shm_pixmap.valid = false;
    }
    out->current_source_kind = WP_I3_SOURCE_NONE;
    out->has_bound_channel = false;

    uint32_t value_list[1] = {0};
    xcb_change_window_attributes(state->conn, out->window, XCB_CW_BACK_PIXEL,
                                 value_list);
    xcb_clear_area(state->conn, 0, out->window, 0, 0, 0, 0);
  }
  xcb_flush(state->conn);
  printf("[ctl] detached, released all buffers\n");
}

static void handle_x_event(wp_i3_state_t *state, xcb_generic_event_t *event) {
  uint8_t response_type = event->response_type & 0x7f;
  if (response_type == XCB_EXPOSE) {
    xcb_expose_event_t *expose = (xcb_expose_event_t *)event;
    if (expose->count == 0) {
      for (size_t i = 0; i < state->output_count; i++) {
        if (state->outputs[i].window == expose->window) {
          refresh_buffer(state, &state->outputs[i]);
          break;
        }
      }
    }
  } else if (response_type == XCB_DESTROY_NOTIFY) {
    xcb_destroy_notify_event_t *destroy = (xcb_destroy_notify_event_t *)event;
    for (size_t i = 0; i < state->output_count; i++) {
      if (state->outputs[i].window == destroy->window) {
        printf("background window destroyed, exiting\n");
        exit(0);
      }
    }
  } else if (response_type == 0) {
    printf("[x11] protocol error\n");
  }
}

static bool capture_channel_readback(wp_i3_state_t *state, uint32_t channel,
                                     uint8_t **out_pixels, int *out_width,
                                     int *out_height, char *err,
                                     size_t err_len) {
  wp_i3_output_t *out = find_output_for_channel(state, channel);
  if (!out || out->current_source_kind != WP_I3_SOURCE_SLOT) {
    snprintf(err, err_len, "no active wallpaper frame on channel %u", channel);
    return false;
  }

  uint32_t local_idx = out->current_source_slot % WP_I3_CAPTURE_SLOT_COUNT;
  wp_i3_slot_t *slot = &out->slots[local_idx];
  if (!slot->in_use || slot->slot != out->current_source_slot) {
    snprintf(err, err_len, "no active wallpaper frame on channel %u", channel);
    return false;
  }

  xcb_get_image_cookie_t cookie =
      xcb_get_image(state->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, slot->pixmap, 0, 0,
                    (uint16_t)slot->width, (uint16_t)slot->height, ~0u);
  xcb_generic_error_t *get_err = NULL;
  xcb_get_image_reply_t *reply =
      xcb_get_image_reply(state->conn, cookie, &get_err);
  if (!reply) {
    snprintf(err, err_len, "%s", "xcb_get_image failed");
    free(get_err);
    return false;
  }

  size_t expected = (size_t)slot->width * slot->height * 4;
  uint8_t *data = xcb_get_image_data(reply);
  size_t data_len = (size_t)xcb_get_image_data_length(reply);
  if (data_len < expected) {
    snprintf(err, err_len, "%s",
             "xcb_get_image returned less data than expected");
    free(reply);
    return false;
  }

  uint8_t *pixels = malloc(expected);
  if (!pixels) {
    snprintf(err, err_len, "%s", "out of memory");
    free(reply);
    return false;
  }

  for (size_t i = 0; i < expected; i += 4) {
    pixels[i + 0] = data[i + 2];
    pixels[i + 1] = data[i + 1];
    pixels[i + 2] = data[i + 0];
    pixels[i + 3] = 0xff;
  }
  free(reply);

  *out_pixels = pixels;
  *out_width = (int)slot->width;
  *out_height = (int)slot->height;
  return true;
}

typedef struct {
  uint8_t *pixels;
  int width;
  int height;
  char path[WP_CTL_CAPTURE_PATH_MAX];
  wp_ctl_listener_t *listener;
} wp_i3_capture_job_t;

static void *capture_encode_and_reply(void *arg) {
  wp_i3_capture_job_t *job = arg;

  wp_ctl_response_t response;
  memset(&response, 0, sizeof(response));
  response.tag = WP_CTL_RESPONSE_OK;
  if (!stbi_write_png(job->path, job->width, job->height, 4, job->pixels,
                      job->width * 4)) {
    response.tag = WP_CTL_RESPONSE_ERR;
    snprintf(response.err, sizeof(response.err),
             "failed to write PNG to %.200s", job->path);
  }

  wp_ctl_listener_reply(job->listener, &response);

  free(job->pixels);
  free(job);
  return NULL;
}

static void handle_ctl_request(wp_i3_state_t *state, wp_ctl_request_t request,
                               wp_ctl_listener_t *listener) {
  wp_ctl_response_t response;
  memset(&response, 0, sizeof(response));

  switch (request) {
  case WP_CTL_REQUEST_GEOMETRY:
    // response.tag = WP_CTL_RESPONSE_GEOMETRY;
    // response.geometry = state->geometry;
    response.tag = WP_CTL_RESPONSE_ERR;
    snprintf(response.err, sizeof(response.err), "%s",
             "geometry detection disabled");
    break;
  case WP_CTL_REQUEST_DETACH:
    detach(state);
    response.tag = WP_CTL_RESPONSE_OK;
    break;
  case WP_CTL_REQUEST_DEBUG_ON:
    set_debug_enabled(state, true);
    response.tag = WP_CTL_RESPONSE_OK;
    break;
  case WP_CTL_REQUEST_DEBUG_OFF:
    set_debug_enabled(state, false);
    response.tag = WP_CTL_RESPONSE_OK;
    break;
  case WP_CTL_REQUEST_CURSOR_POS:
    response.tag = WP_CTL_RESPONSE_ERR;
    snprintf(response.err, sizeof(response.err), "%s",
             "handled by ctl listener");
    break;
  case WP_CTL_REQUEST_PING:
    response.tag = WP_CTL_RESPONSE_OK;
    break;
  case WP_CTL_REQUEST_CAPTURE: {
    uint32_t channel = 0;
    char path[WP_CTL_CAPTURE_PATH_MAX];
    wp_ctl_listener_get_capture_args(listener, &channel, path, sizeof(path));

    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    if (!capture_channel_readback(state, channel, &pixels, &width, &height,
                                  response.err, sizeof(response.err))) {
      response.tag = WP_CTL_RESPONSE_ERR;
      break;
    }

    wp_i3_capture_job_t *job = malloc(sizeof(*job));
    if (!job) {
      free(pixels);
      response.tag = WP_CTL_RESPONSE_ERR;
      snprintf(response.err, sizeof(response.err), "%s", "out of memory");
      break;
    }
    job->pixels = pixels;
    job->width = width;
    job->height = height;
    snprintf(job->path, sizeof(job->path), "%s", path);
    job->listener = listener;

    pthread_t thread;
    if (pthread_create(&thread, NULL, capture_encode_and_reply, job) != 0) {
      free(pixels);
      free(job);
      response.tag = WP_CTL_RESPONSE_ERR;
      snprintf(response.err, sizeof(response.err), "%s",
               "failed to spawn capture encode thread");
      break;
    }
    pthread_detach(thread);
    /* The worker thread replies once the PNG is written -- don't fall
     * through to the trailing wp_ctl_listener_reply() below. */
    return;
  }
  default:
    response.tag = WP_CTL_RESPONSE_ERR;
    snprintf(response.err, sizeof(response.err), "%s", "unrecognized command");
    break;
  }

  wp_ctl_listener_reply(listener, &response);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  wp_i3_state_t state;
  memset(&state, 0, sizeof(state));
  state.stats = wp_frame_stats_create();
  wp_debug_throttle_init(&state.debug_throttle);

  detect_outputs(&state);
  // wp_i3_output_t *primary = &state.outputs[0];
  // state.geometry.x = primary->x;
  // state.geometry.y = primary->y;
  // state.geometry.width = primary->width;
  // state.geometry.height = primary->height;
  // state.geometry.logical_width = primary->width;
  // state.geometry.logical_height = primary->height;
  // state.geometry.scale = 1.0;
  printf("detected %zu output(s)\n", state.output_count);

  int screen_num = 0;
  state.conn = xcb_connect(NULL, &screen_num);
  if (!state.conn || xcb_connection_has_error(state.conn)) {
    fprintf(stderr, "connect to X11: failed\n");
    return 1;
  }

  const xcb_setup_t *setup = xcb_get_setup(state.conn);
  xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screen_num; i++) {
    xcb_screen_next(&screen_iter);
  }
  xcb_screen_t *screen = screen_iter.data;
  state.root = screen->root;
  state.depth = screen->root_depth;
  state.visual = screen->root_visual;

  xcb_generic_error_t *err = NULL;

  xcb_dri3_query_version_cookie_t dri3_cookie =
      xcb_dri3_query_version(state.conn, 1, 2);
  xcb_dri3_query_version_reply_t *dri3_reply =
      xcb_dri3_query_version_reply(state.conn, dri3_cookie, &err);
  if (!dri3_reply) {
    fprintf(stderr, "DRI3 extension not available\n");
    return 1;
  }
  free(dri3_reply);

  xcb_shm_query_version_cookie_t shm_cookie = xcb_shm_query_version(state.conn);
  xcb_shm_query_version_reply_t *shm_reply =
      xcb_shm_query_version_reply(state.conn, shm_cookie, &err);
  if (!shm_reply) {
    fprintf(stderr, "MIT-SHM extension not available\n");
    return 1;
  }
  free(shm_reply);

  for (size_t i = 0; i < state.output_count; i++) {
    wp_i3_output_t *out = &state.outputs[i];

    out->window = xcb_generate_id(state.conn);
    uint32_t win_value_list[4] = {0, XCB_BACKING_STORE_ALWAYS, 1,
                                  XCB_EVENT_MASK_EXPOSURE};
    uint32_t win_value_mask = XCB_CW_BACK_PIXEL | XCB_CW_BACKING_STORE |
                              XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    xcb_void_cookie_t create_cookie = xcb_create_window_checked(
        state.conn, state.depth, out->window, state.root, (int16_t)out->x,
        (int16_t)out->y, (uint16_t)out->width, (uint16_t)out->height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, state.visual, win_value_mask,
        win_value_list);
    xcb_generic_error_t *create_err =
        xcb_request_check(state.conn, create_cookie);
    if (create_err) {
      fprintf(stderr, "create_window failed for output %s\n", out->name);
      free(create_err);
      return 1;
    }

    xcb_map_window(state.conn, out->window);

    uint32_t stack_value_list[1] = {XCB_STACK_MODE_BELOW};
    xcb_configure_window(state.conn, out->window, XCB_CONFIG_WINDOW_STACK_MODE,
                         stack_value_list);

    out->gc = xcb_generate_id(state.conn);
    xcb_create_gc(state.conn, out->gc, out->window, 0, NULL);

    printf("background window %u mapped for output %s at %ux%u+%d+%d\n",
           out->window, out->name, out->width, out->height, out->x, out->y);
  }
  xcb_flush(state.conn);

  int capture_fd = wp_bind_capture_socket();
  wp_ctl_listener_t *ctl_listener =
      wp_ctl_listener_start("i3", cursor_pos_fn, NULL);

  for (;;) {
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(state.conn)) != NULL) {
      handle_x_event(&state, event);
      free(event);
    }

    for (;;) {
      wp_capture_event_t capture_event;
      if (!wp_recv_capture_event(capture_fd, &capture_event)) {
        break;
      }
      handle_capture_event(&state, &capture_event);
    }

    wp_ctl_request_t request;
    if (ctl_listener && wp_ctl_listener_poll(ctl_listener, &request)) {
      handle_ctl_request(&state, request, ctl_listener);
    }

    if (state.debug_enabled) {
      maybe_redraw_debug(&state);
    }

    xcb_flush(state.conn);

    struct pollfd pollfds[2] = {
        {.fd = capture_fd, .events = POLLIN, .revents = 0},
        {.fd = xcb_get_file_descriptor(state.conn),
         .events = POLLIN,
         .revents = 0},
    };
    poll(pollfds, 2, 250);
  }
}
