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
#include <stdint.h>

#include "wallpiper/ctl_protocol.h"
#include "wallpiper/monitor_geometry.h"

typedef bool (*wp_wl_try_geometry_fn)(wp_monitor_geometry_t *out);

typedef struct {
  const char *portal_name;
  const char *layer_namespace;
  wp_wl_try_geometry_fn try_geometry;
  wp_ctl_cursor_pos_fn cursor_pos;
  void *cursor_ctx;
} wp_wl_portal_config_t;

void wp_wl_portal_run(const wp_wl_portal_config_t *config);

void wp_wl_geometry_from_scale(int32_t x, int32_t y, uint32_t width,
                               uint32_t height, double scale,
                               wp_monitor_geometry_t *out);

bool wp_wl_x11_output_rect(const char *name, int32_t *x, int32_t *y,
                           uint32_t *width, uint32_t *height);
