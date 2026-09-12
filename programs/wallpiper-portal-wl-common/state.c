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

#include "state_internal.h"
#include "x11_output_lookup.h"

#include <wallpiper/vk_format.h>

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static wp_wl_output_t *find_output_by_name(wp_wl_state_t *state,
                                           const char *name) {
  if (name[0] == '\0') {
    return NULL;
  }
  for (size_t i = 0; i < state->output_count; i++) {
    wp_wl_output_t *out = &state->outputs[i];
    if (out->known && !out->has_bound_channel && strcmp(out->name, name) == 0) {
      return out;
    }
  }
  return NULL;
}

wp_wl_output_t *wp_wl_find_output_for_channel(wp_wl_state_t *state,
                                              uint32_t channel) {
  for (size_t i = 0; i < state->output_count; i++) {
    wp_wl_output_t *out = &state->outputs[i];
    if (out->has_bound_channel && out->bound_channel == channel) {
      return out;
    }
  }
  return NULL;
}

static wp_wl_output_t *claim_output_for_position(wp_wl_state_t *state,
                                                 uint32_t channel,
                                                 int32_t geom_x,
                                                 int32_t geom_y) {
  char name[64];
  if (wp_wl_x11_output_for_position(geom_x, geom_y, name, sizeof(name))) {
    wp_wl_output_t *out = find_output_by_name(state, name);
    if (out) {
      out->has_bound_channel = true;
      out->bound_channel = channel;
      printf("[socket] channel %u matched by XRandR output name \"%s\"\n",
             channel, name);
      return out;
    }
  }

  for (size_t i = 0; i < state->output_count; i++) {
    wp_wl_output_t *out = &state->outputs[i];
    if (out->known && !out->has_bound_channel && out->x == geom_x &&
        out->y == geom_y) {
      out->has_bound_channel = true;
      out->bound_channel = channel;
      return out;
    }
  }
  return NULL;
}

static bool all_outputs_known(wp_wl_state_t *state) {
  for (size_t i = 0; i < state->output_count; i++) {
    if (!state->outputs[i].known) {
      return false;
    }
  }
  return true;
}

static wp_wl_output_t *claim_output_for_size(wp_wl_state_t *state,
                                             uint32_t channel, uint32_t width,
                                             uint32_t height) {
  char name[64];
  if (wp_wl_x11_output_for_size(width, height, name, sizeof(name))) {
    wp_wl_output_t *out = find_output_by_name(state, name);
    if (out) {
      out->has_bound_channel = true;
      out->bound_channel = channel;
      printf("[socket] channel %u matched by XRandR output name \"%s\"\n",
             channel, name);
      return out;
    }
  }

  for (size_t i = 0; i < state->output_count; i++) {
    wp_wl_output_t *out = &state->outputs[i];
    if (out->known && !out->has_bound_channel && out->width == width &&
        out->height == height) {
      out->has_bound_channel = true;
      out->bound_channel = channel;
      return out;
    }
  }

  if (!all_outputs_known(state)) {
    return NULL;
  }

  wp_wl_output_t *best = NULL;
  for (size_t i = 0; i < state->output_count; i++) {
    wp_wl_output_t *out = &state->outputs[i];
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

static bool try_register_buf(wp_wl_state_t *state, uint32_t channel,
                             uint32_t wire_slot, uint32_t width,
                             uint32_t height, uint32_t format, uint32_t stride,
                             uint64_t modifier, bool has_geometry,
                             int32_t geom_x, int32_t geom_y, int fd) {
  uint32_t local_idx = wire_slot % WP_WL_CAPTURE_SLOT_COUNT;

  wp_wl_output_t *out = wp_wl_find_output_for_channel(state, channel);
  if (!out) {
    if (has_geometry) {
      out = claim_output_for_position(state, channel, geom_x, geom_y);
    }
    if (!out) {
      out = claim_output_for_size(state, channel, width, height);
    }
    if (!out) {
      return false;
    }
    printf("[socket] channel %u bound to output=%p at (%d,%d) for stream "
           "%ux%u%s\n",
           channel, (void *)out->output, out->x, out->y, width, height,
           has_geometry ? " (matched by real window position)" : "");
  }

  wp_wl_slot_t *slot = &out->slots[local_idx];

  struct wl_buffer *buffer = wp_wl_create_dmabuf_buffer(
      state, fd, width, height, format, stride, modifier);
  if (!buffer) {
    close(fd);
    return true;
  }

  if (slot->in_use) {
    wl_buffer_destroy(slot->buffer);
    close(slot->fd);
  }

  slot->in_use = true;
  slot->slot = wire_slot;
  slot->buffer = buffer;
  slot->fd = fd;
  slot->width = width;
  slot->height = height;
  slot->format = format;
  slot->stride = stride;
  slot->modifier = modifier;

  int matched = 0;
  uint32_t fourcc = wp_drm_fourcc_from_vk_format(format, &matched);
  printf("[socket] output=%p registered capture slot %u (channel=%u "
         "local=%u) %ux%u VkFormat=%u -> DRM fourcc 0x%08x%s stride=%u "
         "modifier=%llu\n",
         (void *)out->output, wire_slot, channel, local_idx, width, height,
         format, fourcc, matched ? "" : " (unrecognized, defaulted)", stride,
         (unsigned long long)modifier);

  wp_wl_source_t source = {.kind = WP_WL_SOURCE_SLOT, .slot = wire_slot};
  wp_wl_set_current_source(out, source);
  return true;
}

static void handle_buf(wp_wl_state_t *state, uint32_t wire_slot, uint32_t width,
                       uint32_t height, uint32_t format, uint32_t stride,
                       uint64_t modifier, bool has_geometry, int32_t geom_x,
                       int32_t geom_y, int fd) {
  uint32_t channel = wire_slot / WP_WL_CAPTURE_SLOT_COUNT;
  if (channel >= WP_WL_MAX_CAPTURE_CHANNELS) {
    printf("[socket] bad wire slot %u, dropping\n", wire_slot);
    close(fd);
    return;
  }

  if (try_register_buf(state, channel, wire_slot, width, height, format,
                       stride, modifier, has_geometry, geom_x, geom_y, fd)) {
    return;
  }

  uint32_t local_idx = wire_slot % WP_WL_CAPTURE_SLOT_COUNT;
  wp_wl_pending_buf_t *pending = &state->pending_bufs[channel][local_idx];
  if (pending->pending) {
    close(pending->fd);
  }
  *pending = (wp_wl_pending_buf_t){
      .pending = true,
      .channel = channel,
      .wire_slot = wire_slot,
      .width = width,
      .height = height,
      .format = format,
      .stride = stride,
      .modifier = modifier,
      .has_geometry = has_geometry,
      .geom_x = geom_x,
      .geom_y = geom_y,
      .fd = fd,
  };
  printf("[socket] no output available yet for new channel %u (%ux%u), "
         "queued for retry\n",
         channel, width, height);
}

void wp_wl_retry_pending_bufs(wp_wl_state_t *state) {
  for (uint32_t channel = 0; channel < WP_WL_MAX_CAPTURE_CHANNELS; channel++) {
    for (uint32_t local_idx = 0; local_idx < WP_WL_CAPTURE_SLOT_COUNT;
        local_idx++) {
      wp_wl_pending_buf_t *pending = &state->pending_bufs[channel][local_idx];
      if (!pending->pending) {
        continue;
      }
      if (try_register_buf(state, pending->channel, pending->wire_slot,
                           pending->width, pending->height, pending->format,
                           pending->stride, pending->modifier,
                           pending->has_geometry, pending->geom_x,
                           pending->geom_y, pending->fd)) {
        pending->pending = false;
      }
    }
  }
}

void wp_wl_set_current_source(wp_wl_output_t *out, wp_wl_source_t source) {
  out->current_source = source;
  out->has_current_source = true;
  if (!out->frame_loop_running) {
    out->frame_loop_running = true;
    wp_wl_refresh_buffer(out);
  }
}

void wp_wl_handle_capture_event(wp_wl_state_t *state,
                                const wp_capture_event_t *event) {
  wp_frame_stats_record_capture(state->stats);

  switch (event->tag) {
  case WP_CAPTURE_EVENT_BUF: {
    int image_fd = event->fds[0];
    if (event->nfds > 1) {
      close(event->fds[1]);
    }
    handle_buf(state, event->slot, event->width, event->height,
               event->format, event->stride, event->modifier,
               event->has_geometry, event->geom_x, event->geom_y, image_fd);
    break;
  }
  case WP_CAPTURE_EVENT_FRAME: {
    if (event->nfds > 0) {
      close(event->fds[0]);
    }
    uint32_t channel = event->slot / WP_WL_CAPTURE_SLOT_COUNT;
    uint32_t local_idx = event->slot % WP_WL_CAPTURE_SLOT_COUNT;
    wp_wl_output_t *out = wp_wl_find_output_for_channel(state, channel);
    if (out && out->slots[local_idx].in_use) {
      wp_wl_source_t source = {.kind = WP_WL_SOURCE_SLOT, .slot = event->slot};
      wp_wl_set_current_source(out, source);
    } else if (channel < WP_WL_MAX_CAPTURE_CHANNELS &&
              !state->logged_dropped_frame[channel]) {
      state->logged_dropped_frame[channel] = true;
      printf("[socket] dropping frames for channel %u, output not bound yet "
             "(further drops on this channel won't be logged)\n",
             channel);
    }
    break;
  }
  case WP_CAPTURE_EVENT_SHM: {
    if (state->output_count == 0) {
      close(event->fds[0]);
      break;
    }
    wp_wl_source_t source = {
        .kind = WP_WL_SOURCE_SHM,
        .fd = event->fds[0],
        .width = event->width,
        .height = event->height,
        .stride = event->stride,
    };
    wp_wl_set_current_source(&state->outputs[0], source);
    break;
  }
  }
}

void wp_wl_detach(wp_wl_state_t *state) {
  for (uint32_t channel = 0; channel < WP_WL_MAX_CAPTURE_CHANNELS; channel++) {
    for (uint32_t local_idx = 0; local_idx < WP_WL_CAPTURE_SLOT_COUNT;
        local_idx++) {
      wp_wl_pending_buf_t *pending = &state->pending_bufs[channel][local_idx];
      if (pending->pending) {
        close(pending->fd);
        pending->pending = false;
      }
    }
    state->logged_dropped_frame[channel] = false;
  }

  for (size_t i = 0; i < state->output_count; i++) {
    wp_wl_output_t *out = &state->outputs[i];
    if (out->surface) {
      wl_surface_attach(out->surface, NULL, 0, 0);
      wl_surface_commit(out->surface);
    }
    out->has_current_source = false;
    out->frame_loop_running = false;
    out->has_bound_channel = false;

    if (out->current_shm_buffer) {
      wl_buffer_destroy(out->current_shm_buffer);
      out->current_shm_buffer = NULL;
    }
    if (out->current_shm_pool) {
      wl_shm_pool_destroy(out->current_shm_pool);
      out->current_shm_pool = NULL;
    }

    for (size_t j = 0; j < WP_WL_CAPTURE_SLOT_COUNT; j++) {
      if (out->slots[j].in_use) {
        wl_buffer_destroy(out->slots[j].buffer);
        close(out->slots[j].fd);
        out->slots[j].in_use = false;
      }
    }
  }

  printf("[ctl] detached from compositor, released all buffers\n");
}

void wp_wl_refresh_buffer(wp_wl_output_t *out) {
  if (!out->surface || !out->has_current_source) {
    return;
  }

  struct wl_buffer *buffer = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  struct wl_buffer *next_shm_buffer = NULL;

  if (out->current_source.kind == WP_WL_SOURCE_SLOT) {
    uint32_t local_idx = out->current_source.slot % WP_WL_CAPTURE_SLOT_COUNT;
    wp_wl_slot_t *slot = &out->slots[local_idx];
    if (!slot->in_use || slot->slot != out->current_source.slot) {
      return;
    }
    buffer = slot->buffer;
    width = slot->width;
    height = slot->height;
  } else if (out->current_source.kind == WP_WL_SOURCE_SHM) {
    wp_wl_state_t *state = out->state;
    if (!state->shm) {
      printf("wl_shm not available\n");
      return;
    }
    int32_t pool_size = (int32_t)out->current_source.stride *
                        (int32_t)out->current_source.height;
    struct wl_shm_pool *pool =
        wl_shm_create_pool(state->shm, out->current_source.fd, pool_size);
    struct wl_buffer *shm_buffer = wl_shm_pool_create_buffer(
        pool, 0, (int32_t)out->current_source.width,
        (int32_t)out->current_source.height,
        (int32_t)out->current_source.stride, WL_SHM_FORMAT_XRGB8888);

    if (out->current_shm_pool) {
      wl_shm_pool_destroy(out->current_shm_pool);
    }
    out->current_shm_pool = pool;

    buffer = shm_buffer;
    width = out->current_source.width;
    height = out->current_source.height;
    next_shm_buffer = shm_buffer;
  } else {
    return;
  }

  wl_surface_attach(out->surface, buffer, 0, 0);
  wl_surface_damage_buffer(out->surface, 0, 0, (int32_t)width, (int32_t)height);
  if (out->viewport) {
    wp_viewport_set_destination(out->viewport, (int32_t)out->configured_width,
                                (int32_t)out->configured_height);
  }
  wp_wl_request_frame_callback(out);
  wl_surface_commit(out->surface);

  if (out->current_shm_buffer) {
    wl_buffer_destroy(out->current_shm_buffer);
  }
  out->current_shm_buffer = next_shm_buffer;

  wp_frame_stats_record_display(out->state->stats);
}

void wp_wl_set_debug_enabled(wp_wl_state_t *state, bool enabled) {
  state->debug_enabled = enabled;
  if (enabled) {
    wp_wl_ensure_debug_surface(state);
    wp_debug_throttle_reset(&state->debug_throttle);
  } else if (state->debug_surface) {
    wl_surface_attach(state->debug_surface, NULL, 0, 0);
    wl_surface_commit(state->debug_surface);
  }
  printf("[ctl] debug overlay -> %s\n", enabled ? "true" : "false");
}

void wp_wl_ensure_debug_surface(wp_wl_state_t *state) {
  if (state->debug_surface) {
    return;
  }
  if (!state->subcompositor) {
    printf("[debug] wl_subcompositor not available, cannot create overlay\n");
    return;
  }
  wp_wl_output_t *primary = state->output_count > 0 ? &state->outputs[0] : NULL;
  if (!primary || !primary->surface) {
    return;
  }

  struct wl_surface *surface = wl_compositor_create_surface(state->compositor);
  struct wl_subsurface *subsurface = wl_subcompositor_get_subsurface(
      state->subcompositor, surface, primary->surface);
  int32_t y =
      ((int32_t)primary->configured_height - WP_DEBUG_OVERLAY_HEIGHT) / 2;
  if (y < 0) {
    y = 0;
  }
  wl_subsurface_set_position(subsurface, 12, y);
  wl_subsurface_set_desync(subsurface);
  wl_surface_commit(surface);

  state->debug_surface = surface;
  state->debug_subsurface = subsurface;
}

void wp_wl_maybe_redraw_debug(wp_wl_state_t *state) {
  if (wp_debug_throttle_should_redraw(&state->debug_throttle)) {
    wp_wl_draw_debug_overlay(state);
  }
}

void wp_wl_draw_debug_overlay(wp_wl_state_t *state) {
  wp_wl_ensure_debug_surface(state);
  if (!state->debug_surface || !state->shm) {
    return;
  }

  static uint8_t pixels[WP_DEBUG_OVERLAY_BUFFER_SIZE];
  wp_render_stats_panel(state->stats, pixels);

  int memfd = memfd_create("wallpiper-debug-overlay", 0);
  if (memfd < 0) {
    printf("[debug] memfd_create failed\n");
    return;
  }

  ssize_t written = write(memfd, pixels, sizeof(pixels));
  if (written < 0 || (size_t)written != sizeof(pixels)) {
    printf("[debug] failed to write overlay pixels\n");
    close(memfd);
    return;
  }

  struct wl_shm_pool *pool =
      wl_shm_create_pool(state->shm, memfd, WP_DEBUG_OVERLAY_BUFFER_SIZE);
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(
      pool, 0, WP_DEBUG_OVERLAY_WIDTH, WP_DEBUG_OVERLAY_HEIGHT,
      WP_DEBUG_OVERLAY_STRIDE, WL_SHM_FORMAT_ARGB8888);

  wl_surface_attach(state->debug_surface, buffer, 0, 0);
  wl_surface_damage_buffer(state->debug_surface, 0, 0, WP_DEBUG_OVERLAY_WIDTH,
                           WP_DEBUG_OVERLAY_HEIGHT);
  wl_surface_commit(state->debug_surface);

  if (state->debug_shm_pool) {
    wl_shm_pool_destroy(state->debug_shm_pool);
  }
  state->debug_shm_pool = pool;

  if (state->debug_shm_buffer) {
    wl_buffer_destroy(state->debug_shm_buffer);
  }
  state->debug_shm_buffer = buffer;

  if (state->has_debug_shm_fd) {
    close(state->debug_shm_fd);
  }
  state->debug_shm_fd = memfd;
  state->has_debug_shm_fd = true;
}

void wp_wl_refresh_geometry(wp_wl_state_t *state) {
  wp_monitor_geometry_t geometry;
  wp_wl_detect_geometry(state->config->try_geometry, &geometry);

  if (memcmp(&geometry, &state->geometry, sizeof(geometry)) != 0) {
    printf(
        "monitor geometry changed: x=%d y=%d %ux%u logical=%ux%u scale=%.4f\n",
        geometry.x, geometry.y, geometry.width, geometry.height,
        geometry.logical_width, geometry.logical_height, geometry.scale);
  }
  state->geometry = geometry;
}
