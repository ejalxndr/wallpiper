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
  WP_PORTAL_SPAWN,
  WP_PORTAL_EXTERNALLY_MANAGED,
} wp_portal_strategy_kind_t;

typedef struct {
  wp_portal_strategy_kind_t kind;
  char name[64];
  char binary[1024];
} wp_portal_strategy_t;

void wp_portal_spawn_strategy(const char *name, wp_portal_strategy_t *out);
void wp_portal_spawn(const wp_portal_strategy_t *strategy);
void wp_portal_spawn_readiness_watcher(const char *name, bool patient);
void wp_portal_wait_ready(void);
bool wp_portal_current_monitor(wp_monitor_geometry_t *out);
bool wp_portal_current_render_node(uint32_t *major, uint32_t *minor);
bool wp_portal_query_monitor_once(const char *name, wp_monitor_geometry_t *out);
bool wp_portal_detach_display(void);
void wp_portal_set_debug_overlay(bool enabled);
bool wp_portal_capture_frame(uint32_t channel, const char *path, char *err,
                             size_t err_len);
bool wp_portal_take_display_pid(int *out_pid);
