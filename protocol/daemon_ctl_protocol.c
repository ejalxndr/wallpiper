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

#include "wallpiper/daemon_ctl_protocol.h"
#include "wallpiper/protocol.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static void trim(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

bool wp_daemon_ctl_encode_ok(char *out, size_t out_len) {
  int n = snprintf(out, out_len, "OK\n");
  return n > 0 && (size_t)n < out_len;
}

bool wp_daemon_ctl_encode_err(const char *message, char *out, size_t out_len) {
  int n = snprintf(out, out_len, "ERR %s\n", message);
  return n > 0 && (size_t)n < out_len;
}

bool wp_daemon_ctl_parse_response(const char *line, bool *ok_out, char *err_out,
                                  size_t err_out_len) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "%s", line);
  if (n <= 0 || (size_t)n >= sizeof(buf)) {
    return false;
  }
  trim(buf);

  if (strcmp(buf, "OK") == 0) {
    *ok_out = true;
    if (err_out && err_out_len > 0) {
      err_out[0] = '\0';
    }
    return true;
  }
  if (strncmp(buf, "ERR", 3) == 0) {
    const char *rest = buf + 3;
    while (*rest == ' ' || *rest == '\t') {
      rest++;
    }
    *ok_out = false;
    if (err_out && err_out_len > 0) {
      snprintf(err_out, err_out_len, "%s", rest);
    }
    return true;
  }
  return false;
}

bool wp_send_daemon_command(const char *const *args, size_t arg_count,
                            char *err_out, size_t err_out_len) {
  char path[512];
  if (!wp_daemon_ctl_socket_path(path, sizeof(path))) {
    snprintf(err_out, err_out_len, "could not resolve daemon ctl socket path");
    return false;
  }

  size_t path_len = strlen(path);
  if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    snprintf(err_out, err_out_len, "daemon ctl socket path too long");
    return false;
  }

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    snprintf(err_out, err_out_len, "could not create socket");
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, path_len);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    snprintf(err_out, err_out_len,
             "wallpiperd is not running (no socket at %s)", path);
    close(sock);
    return false;
  }

  bool is_capture = arg_count > 0 && strcmp(args[0], "capture") == 0;
  struct timeval read_tv = {.tv_sec = is_capture ? 15 : 5, .tv_usec = 0};
  struct timeval write_tv = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_tv, sizeof(read_tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &write_tv, sizeof(write_tv));

  char request[1024];
  size_t offset = 0;
  for (size_t i = 0; i < arg_count; i++) {
    int n = snprintf(request + offset, sizeof(request) - offset, "%s%s",
                     i == 0 ? "" : " ", args[i]);
    if (n <= 0 || offset + (size_t)n >= sizeof(request)) {
      snprintf(err_out, err_out_len, "command too long");
      close(sock);
      return false;
    }
    offset += (size_t)n;
  }
  request[offset++] = '\n';

  if (write(sock, request, offset) != (ssize_t)offset) {
    snprintf(err_out, err_out_len, "failed to write command");
    close(sock);
    return false;
  }

  char line[512];
  size_t total = 0;
  while (total + 1 < sizeof(line)) {
    char c;
    ssize_t n = read(sock, &c, 1);
    if (n <= 0) {
      break;
    }
    line[total++] = c;
    if (c == '\n') {
      break;
    }
  }
  line[total] = '\0';
  close(sock);

  if (total == 0) {
    snprintf(err_out, err_out_len, "no response from wallpiperd");
    return false;
  }

  bool ok;
  char parsed_err[256];
  if (!wp_daemon_ctl_parse_response(line, &ok, parsed_err,
                                    sizeof(parsed_err))) {
    snprintf(err_out, err_out_len, "malformed response from wallpiperd: %s",
             line);
    return false;
  }
  if (!ok) {
    snprintf(err_out, err_out_len, "%s", parsed_err);
    return false;
  }
  return true;
}
