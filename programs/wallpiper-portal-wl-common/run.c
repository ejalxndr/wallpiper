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

#include "egl_capture.h"
#include "state_internal.h"

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stb_image_write.h"

void wp_wl_geometry_from_scale(int32_t x, int32_t y, uint32_t width,
                               uint32_t height, double scale,
                               wp_monitor_geometry_t *out) {
  if (scale <= 0.0) {
    scale = 1.0;
  }
  out->x = x;
  out->y = y;
  out->width = width;
  out->height = height;
  out->logical_width = (uint32_t)llround((double)width / scale);
  out->logical_height = (uint32_t)llround((double)height / scale);
  out->scale = scale;
}

bool wp_wl_detect_geometry(wp_wl_try_geometry_fn try_fn,
                           wp_monitor_geometry_t *out) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (try_fn(out)) {
      return true;
    }
    printf("monitor detection attempt %d failed, retrying\n", attempt);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
  printf("monitor detection failed after retries, falling back to 1920x1080 at "
         "0,0\n");
  out->x = 0;
  out->y = 0;
  out->width = 1920;
  out->height = 1080;
  out->logical_width = 1920;
  out->logical_height = 1080;
  out->scale = 1.0;
  return true;
}

typedef struct {
  uint8_t *pixels;
  int width;
  int height;
  char path[WP_CTL_CAPTURE_PATH_MAX];
  wp_ctl_listener_t *listener;
  uint32_t generation;
} wp_wl_capture_job_t;

static void *wp_wl_capture_encode_and_reply(void *arg) {
  wp_wl_capture_job_t *job = arg;

  wp_ctl_response_t response;
  memset(&response, 0, sizeof(response));
  response.tag = WP_CTL_RESPONSE_OK;
  if (!stbi_write_png(job->path, job->width, job->height, 4, job->pixels,
                      job->width * 4)) {
    response.tag = WP_CTL_RESPONSE_ERR;
    snprintf(response.err, sizeof(response.err),
             "failed to write PNG to %.200s", job->path);
  }

  wp_ctl_listener_reply(job->listener, job->generation, &response);

  free(job->pixels);
  free(job);
  return NULL;
}

static void handle_ctl_request(wp_wl_state_t *state, wp_ctl_request_t request) {
  uint32_t generation = wp_ctl_listener_pending_generation(state->ctl_listener);
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
    wp_wl_detach(state);
    response.tag = WP_CTL_RESPONSE_OK;
    break;
  case WP_CTL_REQUEST_DEBUG_ON:
    wp_wl_set_debug_enabled(state, true);
    response.tag = WP_CTL_RESPONSE_OK;
    break;
  case WP_CTL_REQUEST_DEBUG_OFF:
    wp_wl_set_debug_enabled(state, false);
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
  case WP_CTL_REQUEST_RENDER_NODE:
    if (wp_wl_egl_render_node(&response.render_node_major,
                              &response.render_node_minor)) {
      response.tag = WP_CTL_RESPONSE_RENDER_NODE;
    } else {
      response.tag = WP_CTL_RESPONSE_ERR;
      snprintf(response.err, sizeof(response.err), "%s",
               "render node detection failed");
    }
    break;
  case WP_CTL_REQUEST_CAPTURE: {
    uint32_t channel = 0;
    char path[WP_CTL_CAPTURE_PATH_MAX];
    wp_ctl_listener_get_capture_args(state->ctl_listener, &channel, path,
                                     sizeof(path));

    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    if (!wp_wl_egl_capture_readback(state, channel, &pixels, &width, &height,
                                    response.err, sizeof(response.err))) {
      response.tag = WP_CTL_RESPONSE_ERR;
      break;
    }

    wp_wl_capture_job_t *job = malloc(sizeof(*job));
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
    job->listener = state->ctl_listener;
    job->generation = generation;

    pthread_t thread;
    if (pthread_create(&thread, NULL, wp_wl_capture_encode_and_reply, job) !=
        0) {
      free(pixels);
      free(job);
      response.tag = WP_CTL_RESPONSE_ERR;
      snprintf(response.err, sizeof(response.err), "%s",
               "failed to spawn capture encode thread");
      break;
    }
    pthread_detach(thread);
    return;
  }
  default:
    response.tag = WP_CTL_RESPONSE_ERR;
    snprintf(response.err, sizeof(response.err), "%s", "unrecognized command");
    break;
  }

  wp_ctl_listener_reply(state->ctl_listener, generation, &response);
}

void wp_wl_portal_run(const wp_wl_portal_config_t *config) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  wp_wl_state_t state;
  memset(&state, 0, sizeof(state));
  state.config = config;
  state.debug_shm_fd = -1;
  state.stats = wp_frame_stats_create();
  wp_debug_throttle_init(&state.debug_throttle);

  // wp_wl_detect_geometry(config->try_geometry, &state.geometry);
  // printf(
  //     "detected monitor geometry: x=%d y=%d %ux%u logical=%ux%u
  //     scale=%.4f\n", state.geometry.x, state.geometry.y,
  //     state.geometry.width, state.geometry.height,
  //     state.geometry.logical_width, state.geometry.logical_height,
  //     state.geometry.scale);

  state.display = wl_display_connect(NULL);
  if (!state.display) {
    fprintf(stderr, "connect to wayland: failed\n");
    exit(1);
  }

  wp_wl_bind_globals(&state);

  printf("bound %zu output(s); layer surfaces are created per-output as each "
         "one's geometry becomes known\n",
         state.output_count);

  state.capture_fd = wp_bind_capture_socket();
  state.ctl_listener = wp_ctl_listener_start(
      config->portal_name, config->cursor_pos, config->cursor_ctx);

  for (;;) {
    wl_display_dispatch_pending(state.display);
    wl_display_flush(state.display);
    wp_wl_retry_pending_bufs(&state);

    for (;;) {
      wp_capture_event_t event;
      if (!wp_recv_capture_event(state.capture_fd, &event)) {
        break;
      }
      wp_wl_handle_capture_event(&state, &event);
    }

    wp_ctl_request_t request;
    if (state.ctl_listener &&
        wp_ctl_listener_poll(state.ctl_listener, &request)) {
      handle_ctl_request(&state, request);
    }

    if (state.debug_enabled) {
      wp_wl_maybe_redraw_debug(&state);
    }

    struct pollfd pollfds[2] = {
        {.fd = state.capture_fd, .events = POLLIN, .revents = 0},
        {.fd = wl_display_get_fd(state.display),
         .events = POLLIN,
         .revents = 0},
    };
    poll(pollfds, 2, 250);

    if (pollfds[1].revents & POLLIN) {
      wl_display_dispatch(state.display);
    }
  }
}
