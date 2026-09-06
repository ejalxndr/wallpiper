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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wallpiper/monitor_geometry.h"

typedef enum {
  WP_CTL_REQUEST_GEOMETRY,
  WP_CTL_REQUEST_DETACH,
  WP_CTL_REQUEST_DEBUG_ON,
  WP_CTL_REQUEST_DEBUG_OFF,
  WP_CTL_REQUEST_CURSOR_POS,
  WP_CTL_REQUEST_PING,
  WP_CTL_REQUEST_CAPTURE,
  WP_CTL_REQUEST_RENDER_NODE,
} wp_ctl_request_t;

#define WP_CTL_CAPTURE_PATH_MAX 480

typedef enum {
  WP_CTL_RESPONSE_OK,
  WP_CTL_RESPONSE_ERR,
  WP_CTL_RESPONSE_GEOMETRY,
  WP_CTL_RESPONSE_CURSOR_POS,
  WP_CTL_RESPONSE_RENDER_NODE,
} wp_ctl_response_tag_t;

typedef struct {
  wp_ctl_response_tag_t tag;
  char err[256];
  wp_monitor_geometry_t geometry;
  int32_t cursor_x;
  int32_t cursor_y;
  uint32_t render_node_major;
  uint32_t render_node_minor;
} wp_ctl_response_t;

bool wp_ctl_request_encode(wp_ctl_request_t request, char *out, size_t out_len);
bool wp_ctl_request_parse(const char *line, wp_ctl_request_t *out);

/* Only valid to call after wp_ctl_request_parse() returns
 * WP_CTL_REQUEST_CAPTURE for the same line. */
bool wp_ctl_capture_args_parse(const char *line, uint32_t *channel, char *path,
                               size_t path_len);
bool wp_ctl_request_encode_capture(uint32_t channel, const char *path,
                                   char *out, size_t out_len);

bool wp_ctl_response_encode(const wp_ctl_response_t *response, char *out,
                            size_t out_len);
bool wp_ctl_response_parse(const char *line, wp_ctl_response_t *out);

bool wp_send_ctl_request(const char *portal_name, wp_ctl_request_t request,
                         wp_ctl_response_t *out);
bool wp_send_ctl_capture_request(const char *portal_name, uint32_t channel,
                                 const char *path, wp_ctl_response_t *out);

typedef struct wp_ctl_listener wp_ctl_listener_t;
typedef void (*wp_ctl_cursor_pos_fn)(void *ctx, wp_ctl_response_t *out);

wp_ctl_listener_t *wp_ctl_listener_start(const char *portal_name,
                                         wp_ctl_cursor_pos_fn cursor_fn,
                                         void *cursor_ctx);
void wp_ctl_listener_stop(wp_ctl_listener_t *listener);

bool wp_ctl_listener_poll(wp_ctl_listener_t *listener,
                          wp_ctl_request_t *out_request);
/* Only valid to call after wp_ctl_listener_poll() returns true with
 * *out_request == WP_CTL_REQUEST_CAPTURE, before replying. */
void wp_ctl_listener_get_capture_args(wp_ctl_listener_t *listener,
                                      uint32_t *out_channel, char *out_path,
                                      size_t out_path_len);
void wp_ctl_listener_reply(wp_ctl_listener_t *listener,
                           const wp_ctl_response_t *response);
