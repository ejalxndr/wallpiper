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

#include "commands.h"

#include "portal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool wp_commands_dispatch(const char *const *args, size_t arg_count,
                          char *err_out, size_t err_out_len) {
  if (arg_count == 0) {
    snprintf(err_out, err_out_len, "empty command");
    return false;
  }

  const char *cmd = args[0];

  if (strcmp(cmd, "debug-on") == 0) {
    wp_portal_set_debug_overlay(true);
    return true;
  }
  if (strcmp(cmd, "debug-off") == 0) {
    wp_portal_set_debug_overlay(false);
    return true;
  }
  if (strcmp(cmd, "capture") == 0) {
    if (arg_count < 3) {
      snprintf(err_out, err_out_len, "usage: capture <channel> <path>");
      return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long channel = strtoul(args[1], &end, 10);
    if (end == args[1] || *end != '\0' || errno == ERANGE ||
        channel > UINT32_MAX) {
      snprintf(err_out, err_out_len, "invalid capture channel: %s", args[1]);
      return false;
    }
    return wp_portal_capture_frame((uint32_t)channel, args[2], err_out,
                                   err_out_len);
  }

  snprintf(err_out, err_out_len, "unknown command: %s", cmd);
  return false;
}
