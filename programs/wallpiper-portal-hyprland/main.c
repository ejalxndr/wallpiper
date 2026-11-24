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

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cJSON.h>

#include "wallpiper-wl/portal.h"
#include "wallpiper/monitor_geometry.h"

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

static bool try_detect_geometry(wp_monitor_geometry_t *out) {
  const char *argv[] = {"hyprctl", "monitors", "-j", NULL};
  char *output = run_command(argv);
  if (!output) {
    return false;
  }

  cJSON *monitors = cJSON_Parse(output);
  free(output);
  if (!monitors || !cJSON_IsArray(monitors)) {
    cJSON_Delete(monitors);
    return false;
  }

  bool found = false;
  int count = cJSON_GetArraySize(monitors);
  for (int i = 0; i < count; i++) {
    cJSON *monitor = cJSON_GetArrayItem(monitors, i);
    cJSON *focused = cJSON_GetObjectItem(monitor, "focused");
    if (!focused || !cJSON_IsTrue(focused)) {
      continue;
    }

    cJSON *x = cJSON_GetObjectItem(monitor, "x");
    cJSON *y = cJSON_GetObjectItem(monitor, "y");
    cJSON *width = cJSON_GetObjectItem(monitor, "width");
    cJSON *height = cJSON_GetObjectItem(monitor, "height");
    cJSON *scale = cJSON_GetObjectItem(monitor, "scale");
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(width) ||
        !cJSON_IsNumber(height)) {
      break;
    }

    double scale_value = cJSON_IsNumber(scale) ? scale->valuedouble : 1.0;
    wp_wl_geometry_from_scale((int32_t)x->valuedouble, (int32_t)y->valuedouble,
                              (uint32_t)width->valuedouble,
                              (uint32_t)height->valuedouble, scale_value, out);
    found = true;
    break;
  }

  cJSON_Delete(monitors);
  return found;
}

static char *hypr_socket_path(void) {
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!runtime_dir || !sig) {
    return NULL;
  }

  size_t len = strlen(runtime_dir) + strlen(sig) + 32;
  char *path = malloc(len);
  if (!path) {
    return NULL;
  }
  snprintf(path, len, "%s/hypr/%s/.socket.sock", runtime_dir, sig);
  return path;
}

static char *hypr_request(const char *request) {
  char *path = hypr_socket_path();
  if (!path) {
    return NULL;
  }

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    free(path);
    return NULL;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t path_len = strlen(path);
  bool ok = path_len < sizeof(addr.sun_path);
  if (ok) {
    memcpy(addr.sun_path, path, path_len);
  }
  free(path);
  if (!ok || connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return NULL;
  }

  struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (write(sock, request, strlen(request)) != (ssize_t)strlen(request)) {
    close(sock);
    return NULL;
  }

  size_t cap = 4096;
  size_t total = 0;
  char *response = malloc(cap);
  if (!response) {
    close(sock);
    return NULL;
  }
  for (;;) {
    if (total + 1024 > cap) {
      cap *= 2;
      char *grown = realloc(response, cap);
      if (!grown) {
        free(response);
        close(sock);
        return NULL;
      }
      response = grown;
    }
    ssize_t n = read(sock, response + total, cap - 1 - total);
    if (n <= 0) {
      break;
    }
    total += (size_t)n;
  }
  close(sock);
  response[total] = '\0';
  return response;
}

static void hypr_to_x11_cursor(int32_t *x, int32_t *y) {
  char *output = hypr_request("j/monitors");
  if (!output) {
    return;
  }
  cJSON *monitors = cJSON_Parse(output);
  free(output);
  if (!monitors || !cJSON_IsArray(monitors)) {
    cJSON_Delete(monitors);
    return;
  }

  int count = cJSON_GetArraySize(monitors);
  for (int i = 0; i < count; i++) {
    cJSON *monitor = cJSON_GetArrayItem(monitors, i);
    cJSON *name = cJSON_GetObjectItem(monitor, "name");
    cJSON *mx = cJSON_GetObjectItem(monitor, "x");
    cJSON *my = cJSON_GetObjectItem(monitor, "y");
    cJSON *width = cJSON_GetObjectItem(monitor, "width");
    cJSON *height = cJSON_GetObjectItem(monitor, "height");
    cJSON *scale = cJSON_GetObjectItem(monitor, "scale");
    cJSON *transform = cJSON_GetObjectItem(monitor, "transform");
    if (!cJSON_IsString(name) || !cJSON_IsNumber(mx) || !cJSON_IsNumber(my) ||
        !cJSON_IsNumber(width) || !cJSON_IsNumber(height)) {
      continue;
    }

    double s = cJSON_IsNumber(scale) && scale->valuedouble > 0.0
                   ? scale->valuedouble
                   : 1.0;
    bool rotated = cJSON_IsNumber(transform) && (transform->valueint & 1);
    double lw = (rotated ? height->valuedouble : width->valuedouble) / s;
    double lh = (rotated ? width->valuedouble : height->valuedouble) / s;
    double rx = *x - mx->valuedouble;
    double ry = *y - my->valuedouble;
    if (rx < 0 || ry < 0 || rx >= lw || ry >= lh) {
      continue;
    }

    int32_t ox, oy;
    uint32_t ow, oh;
    if (wp_wl_x11_output_rect(name->valuestring, &ox, &oy, &ow, &oh)) {
      *x = ox + (int32_t)lround(rx * ow / lw);
      *y = oy + (int32_t)lround(ry * oh / lh);
    }
    break;
  }

  cJSON_Delete(monitors);
}

static bool query_cursor_pos(int32_t *out_x, int32_t *out_y) {
  char *response = hypr_request("cursorpos");
  if (!response) {
    return false;
  }

  int x, y;
  bool ok = sscanf(response, "%d, %d", &x, &y) == 2;
  free(response);
  if (!ok) {
    return false;
  }
  *out_x = x;
  *out_y = y;
  hypr_to_x11_cursor(out_x, out_y);
  return true;
}

static void cursor_pos_fn(void *ctx, wp_ctl_response_t *out) {
  (void)ctx;
  memset(out, 0, sizeof(*out));

  int32_t x, y;
  if (query_cursor_pos(&x, &y)) {
    out->tag = WP_CTL_RESPONSE_CURSOR_POS;
    out->cursor_x = x;
    out->cursor_y = y;
  } else {
    out->tag = WP_CTL_RESPONSE_ERR;
    snprintf(out->err, sizeof(out->err), "%s", "cursor position unavailable");
  }
}

int main(void) {
  wp_wl_portal_config_t config = {
      .portal_name = "hyprland",
      .layer_namespace = "wallpiper-portal-hyprland",
      .try_geometry = try_detect_geometry,
      .cursor_pos = cursor_pos_fn,
      .cursor_ctx = NULL,
  };
  wp_wl_portal_run(&config);
  return 0;
}
