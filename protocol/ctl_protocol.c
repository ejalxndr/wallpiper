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

#include "wallpiper/ctl_protocol.h"
#include "wallpiper/protocol.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void trim(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                     s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }
  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t') {
    start++;
  }
  if (start > 0) {
    memmove(s, s + start, len - start + 1);
  }
}

bool wp_ctl_request_encode(wp_ctl_request_t request, char *out,
                           size_t out_len) {
  const char *text;
  switch (request) {
  case WP_CTL_REQUEST_GEOMETRY:
    text = "GEOMETRY\n";
    break;
  case WP_CTL_REQUEST_DETACH:
    text = "DETACH\n";
    break;
  case WP_CTL_REQUEST_DEBUG_ON:
    text = "DEBUG_ON\n";
    break;
  case WP_CTL_REQUEST_DEBUG_OFF:
    text = "DEBUG_OFF\n";
    break;
  case WP_CTL_REQUEST_CURSOR_POS:
    text = "CURSOR_POS\n";
    break;
  case WP_CTL_REQUEST_PING:
    text = "PING\n";
    break;
  case WP_CTL_REQUEST_RENDER_NODE:
    text = "RENDER_NODE\n";
    break;
  default:
    return false;
  }
  int n = snprintf(out, out_len, "%s", text);
  return n > 0 && (size_t)n < out_len;
}

bool wp_ctl_request_encode_capture(uint32_t channel, const char *path,
                                   char *out, size_t out_len) {
  int n = snprintf(out, out_len, "CAPTURE %u %s\n", channel, path);
  return n > 0 && (size_t)n < out_len;
}

bool wp_ctl_request_parse(const char *line, wp_ctl_request_t *out) {
  char buf[WP_CTL_CAPTURE_PATH_MAX + 32];
  int n = snprintf(buf, sizeof(buf), "%s", line);
  if (n <= 0 || (size_t)n >= sizeof(buf)) {
    return false;
  }
  trim(buf);

  if (strcmp(buf, "GEOMETRY") == 0) {
    *out = WP_CTL_REQUEST_GEOMETRY;
  } else if (strcmp(buf, "DETACH") == 0) {
    *out = WP_CTL_REQUEST_DETACH;
  } else if (strcmp(buf, "DEBUG_ON") == 0) {
    *out = WP_CTL_REQUEST_DEBUG_ON;
  } else if (strcmp(buf, "DEBUG_OFF") == 0) {
    *out = WP_CTL_REQUEST_DEBUG_OFF;
  } else if (strcmp(buf, "CURSOR_POS") == 0) {
    *out = WP_CTL_REQUEST_CURSOR_POS;
  } else if (strcmp(buf, "PING") == 0) {
    *out = WP_CTL_REQUEST_PING;
  } else if (strcmp(buf, "RENDER_NODE") == 0) {
    *out = WP_CTL_REQUEST_RENDER_NODE;
  } else if (strncmp(buf, "CAPTURE ", 8) == 0) {
    *out = WP_CTL_REQUEST_CAPTURE;
  } else {
    return false;
  }
  return true;
}

bool wp_ctl_capture_args_parse(const char *line, uint32_t *channel, char *path,
                               size_t path_len) {
  char buf[WP_CTL_CAPTURE_PATH_MAX + 32];
  int n = snprintf(buf, sizeof(buf), "%s", line);
  if (n <= 0 || (size_t)n >= sizeof(buf)) {
    return false;
  }
  trim(buf);

  if (strncmp(buf, "CAPTURE ", 8) != 0) {
    return false;
  }
  const char *rest = buf + 8;
  while (*rest == ' ' || *rest == '\t') {
    rest++;
  }
  char *end = NULL;
  errno = 0;
  unsigned long parsed_channel = strtoul(rest, &end, 10);
  if (end == rest || *end != ' ' || errno == ERANGE ||
      parsed_channel > UINT32_MAX) {
    return false;
  }
  while (*end == ' ' || *end == '\t') {
    end++;
  }
  if (*end == '\0') {
    return false;
  }
  *channel = (uint32_t)parsed_channel;
  int path_n = snprintf(path, path_len, "%s", end);
  if (path_n <= 0 || (size_t)path_n >= path_len) {
    return false;
  }
  return true;
}

bool wp_ctl_response_encode(const wp_ctl_response_t *response, char *out,
                            size_t out_len) {
  int n;
  switch (response->tag) {
  case WP_CTL_RESPONSE_OK:
    n = snprintf(out, out_len, "OK\n");
    break;
  case WP_CTL_RESPONSE_ERR:
    n = snprintf(out, out_len, "ERR %s\n", response->err);
    break;
  case WP_CTL_RESPONSE_GEOMETRY: {
    char json[256];
    if (!wp_monitor_geometry_encode_json(&response->geometry, json,
                                         sizeof(json))) {
      return false;
    }
    n = snprintf(out, out_len, "GEOMETRY %s\n", json);
    break;
  }
  case WP_CTL_RESPONSE_CURSOR_POS:
    n = snprintf(out, out_len, "CURSOR_POS %d %d\n", response->cursor_x,
                 response->cursor_y);
    break;
  case WP_CTL_RESPONSE_RENDER_NODE:
    n = snprintf(out, out_len, "RENDER_NODE %u %u\n",
                 response->render_node_major, response->render_node_minor);
    break;
  default:
    return false;
  }
  return n > 0 && (size_t)n < out_len;
}

bool wp_ctl_response_parse(const char *line, wp_ctl_response_t *out) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "%s", line);
  if (n <= 0 || (size_t)n >= sizeof(buf)) {
    return false;
  }
  trim(buf);

  memset(out, 0, sizeof(*out));

  if (strcmp(buf, "OK") == 0) {
    out->tag = WP_CTL_RESPONSE_OK;
    return true;
  }
  if (strncmp(buf, "ERR", 3) == 0) {
    const char *rest = buf + 3;
    while (*rest == ' ' || *rest == '\t') {
      rest++;
    }
    out->tag = WP_CTL_RESPONSE_ERR;
    snprintf(out->err, sizeof(out->err), "%.*s", (int)sizeof(out->err) - 1,
             rest);
    return true;
  }
  if (strncmp(buf, "GEOMETRY ", 9) == 0) {
    if (!wp_monitor_geometry_decode_json(buf + 9, &out->geometry)) {
      return false;
    }
    out->tag = WP_CTL_RESPONSE_GEOMETRY;
    return true;
  }
  if (strncmp(buf, "CURSOR_POS ", 11) == 0) {
    int x, y;
    if (sscanf(buf + 11, "%d %d", &x, &y) != 2) {
      return false;
    }
    out->tag = WP_CTL_RESPONSE_CURSOR_POS;
    out->cursor_x = x;
    out->cursor_y = y;
    return true;
  }
  if (strncmp(buf, "RENDER_NODE ", 12) == 0) {
    unsigned major, minor;
    if (sscanf(buf + 12, "%u %u", &major, &minor) != 2) {
      return false;
    }
    out->tag = WP_CTL_RESPONSE_RENDER_NODE;
    out->render_node_major = (uint32_t)major;
    out->render_node_minor = (uint32_t)minor;
    return true;
  }
  return false;
}

static int connect_unix_stream(const char *path) {
  size_t path_len = strlen(path);
  if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    return -1;
  }

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, path_len);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

static bool set_timeouts(int sock, long read_secs, long write_secs) {
  struct timeval rcv = {.tv_sec = read_secs, .tv_usec = 0};
  struct timeval snd = {.tv_sec = write_secs, .tv_usec = 0};
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv)) < 0) {
    return false;
  }
  if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)) < 0) {
    return false;
  }
  return true;
}

static bool read_line(int sock, char *out, size_t out_len) {
  size_t total = 0;
  while (total + 1 < out_len) {
    char c;
    ssize_t n = read(sock, &c, 1);
    if (n <= 0) {
      break;
    }
    out[total++] = c;
    if (c == '\n') {
      break;
    }
  }
  out[total] = '\0';
  return total > 0;
}

bool wp_send_ctl_request(const char *portal_name, wp_ctl_request_t request,
                         wp_ctl_response_t *out) {
  char path[256];
  if (!wp_ctl_socket_path(portal_name, path, sizeof(path))) {
    return false;
  }

  int sock = connect_unix_stream(path);
  if (sock < 0) {
    return false;
  }
  if (!set_timeouts(sock, 3, 1)) {
    close(sock);
    return false;
  }

  char req_line[32];
  if (!wp_ctl_request_encode(request, req_line, sizeof(req_line))) {
    close(sock);
    return false;
  }
  size_t req_len = strlen(req_line);
  if (write(sock, req_line, req_len) != (ssize_t)req_len) {
    close(sock);
    return false;
  }

  char resp_line[512];
  bool got_line = read_line(sock, resp_line, sizeof(resp_line));
  close(sock);
  if (!got_line) {
    return false;
  }

  return wp_ctl_response_parse(resp_line, out);
}

bool wp_send_ctl_capture_request(const char *portal_name, uint32_t channel,
                                 const char *path, wp_ctl_response_t *out) {
  char sock_path[256];
  if (!wp_ctl_socket_path(portal_name, sock_path, sizeof(sock_path))) {
    return false;
  }

  int sock = connect_unix_stream(sock_path);
  if (sock < 0) {
    return false;
  }
  if (!set_timeouts(sock, 10, 1)) {
    close(sock);
    return false;
  }

  char req_line[WP_CTL_CAPTURE_PATH_MAX + 32];
  if (!wp_ctl_request_encode_capture(channel, path, req_line,
                                     sizeof(req_line))) {
    close(sock);
    return false;
  }
  size_t req_len = strlen(req_line);
  if (write(sock, req_line, req_len) != (ssize_t)req_len) {
    close(sock);
    return false;
  }

  char resp_line[512];
  bool got_line = read_line(sock, resp_line, sizeof(resp_line));
  close(sock);
  if (!got_line) {
    return false;
  }

  return wp_ctl_response_parse(resp_line, out);
}

#define WP_CTL_MAX_PENDING_LINE 512

struct wp_ctl_listener {
  int listen_fd;
  pthread_t thread;
  volatile bool running;

  wp_ctl_cursor_pos_fn cursor_fn;
  void *cursor_ctx;

  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool has_pending;
  bool delivered;
  bool has_reply;
  uint32_t generation;
  wp_ctl_request_t pending_request;
  uint32_t pending_capture_channel;
  char pending_capture_path[WP_CTL_CAPTURE_PATH_MAX];
  wp_ctl_response_t pending_reply;
};

static void *ctl_listener_thread_main(void *arg) {
  wp_ctl_listener_t *listener = arg;

  while (listener->running) {
    int conn = accept(listener->listen_fd, NULL, NULL);
    if (conn < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    char line[WP_CTL_MAX_PENDING_LINE];
    if (!read_line(conn, line, sizeof(line))) {
      close(conn);
      continue;
    }

    wp_ctl_request_t request;
    if (!wp_ctl_request_parse(line, &request)) {
      wp_ctl_response_t err = {.tag = WP_CTL_RESPONSE_ERR};
      snprintf(err.err, sizeof(err.err), "%s", "unrecognized command");
      char resp[64];
      if (wp_ctl_response_encode(&err, resp, sizeof(resp))) {
        write(conn, resp, strlen(resp));
      }
      close(conn);
      continue;
    }

    uint32_t capture_channel = 0;
    char capture_path[WP_CTL_CAPTURE_PATH_MAX];
    capture_path[0] = '\0';
    if (request == WP_CTL_REQUEST_CAPTURE &&
        !wp_ctl_capture_args_parse(line, &capture_channel, capture_path,
                                   sizeof(capture_path))) {
      wp_ctl_response_t err = {.tag = WP_CTL_RESPONSE_ERR};
      snprintf(err.err, sizeof(err.err), "%s", "malformed capture request");
      char resp[64];
      if (wp_ctl_response_encode(&err, resp, sizeof(resp))) {
        write(conn, resp, strlen(resp));
      }
      close(conn);
      continue;
    }

    wp_ctl_response_t response;
    if (request == WP_CTL_REQUEST_CURSOR_POS && listener->cursor_fn) {
      listener->cursor_fn(listener->cursor_ctx, &response);
    } else {
      pthread_mutex_lock(&listener->mutex);
      listener->pending_request = request;
      listener->pending_capture_channel = capture_channel;
      snprintf(listener->pending_capture_path,
               sizeof(listener->pending_capture_path), "%s", capture_path);
      listener->has_pending = true;
      listener->delivered = false;
      listener->has_reply = false;
      listener->generation++;
      pthread_cond_broadcast(&listener->cond);

      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += request == WP_CTL_REQUEST_CAPTURE ? 8 : 2;

      while (!listener->has_reply) {
        int rc = pthread_cond_timedwait(&listener->cond, &listener->mutex,
                                        &deadline);
        if (rc != 0) {
          break;
        }
      }

      if (listener->has_reply) {
        response = listener->pending_reply;
      } else {
        memset(&response, 0, sizeof(response));
        response.tag = WP_CTL_RESPONSE_ERR;
        snprintf(response.err, sizeof(response.err), "%s",
                 "timed out waiting for response");
      }
      listener->has_pending = false;
      listener->has_reply = false;
      pthread_mutex_unlock(&listener->mutex);
    }

    char resp[512];
    if (wp_ctl_response_encode(&response, resp, sizeof(resp))) {
      write(conn, resp, strlen(resp));
    }
    close(conn);
  }

  return NULL;
}

wp_ctl_listener_t *wp_ctl_listener_start(const char *portal_name,
                                         wp_ctl_cursor_pos_fn cursor_fn,
                                         void *cursor_ctx) {
  char path[256];
  if (!wp_ctl_socket_path(portal_name, path, sizeof(path))) {
    return NULL;
  }

  unlink(path);

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    return NULL;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t path_len = strlen(path);
  if (path_len >= sizeof(addr.sun_path)) {
    close(sock);
    return NULL;
  }
  memcpy(addr.sun_path, path, path_len);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return NULL;
  }
  if (listen(sock, 8) < 0) {
    close(sock);
    return NULL;
  }

  wp_ctl_listener_t *listener = calloc(1, sizeof(wp_ctl_listener_t));
  if (!listener) {
    close(sock);
    return NULL;
  }
  listener->listen_fd = sock;
  listener->cursor_fn = cursor_fn;
  listener->cursor_ctx = cursor_ctx;
  listener->running = true;
  pthread_mutex_init(&listener->mutex, NULL);
  pthread_cond_init(&listener->cond, NULL);

  if (pthread_create(&listener->thread, NULL, ctl_listener_thread_main,
                     listener) != 0) {
    close(sock);
    pthread_mutex_destroy(&listener->mutex);
    pthread_cond_destroy(&listener->cond);
    free(listener);
    return NULL;
  }

  return listener;
}

void wp_ctl_listener_stop(wp_ctl_listener_t *listener) {
  if (!listener) {
    return;
  }
  listener->running = false;
  shutdown(listener->listen_fd, SHUT_RDWR);
  close(listener->listen_fd);
  pthread_join(listener->thread, NULL);
  pthread_mutex_destroy(&listener->mutex);
  pthread_cond_destroy(&listener->cond);
  free(listener);
}

bool wp_ctl_listener_poll(wp_ctl_listener_t *listener,
                          wp_ctl_request_t *out_request) {
  bool found = false;
  pthread_mutex_lock(&listener->mutex);
  if (listener->has_pending && !listener->delivered) {
    *out_request = listener->pending_request;
    listener->delivered = true;
    found = true;
  }
  pthread_mutex_unlock(&listener->mutex);
  return found;
}

void wp_ctl_listener_get_capture_args(wp_ctl_listener_t *listener,
                                      uint32_t *out_channel, char *out_path,
                                      size_t out_path_len) {
  pthread_mutex_lock(&listener->mutex);
  *out_channel = listener->pending_capture_channel;
  snprintf(out_path, out_path_len, "%s", listener->pending_capture_path);
  pthread_mutex_unlock(&listener->mutex);
}

uint32_t wp_ctl_listener_pending_generation(wp_ctl_listener_t *listener) {
  pthread_mutex_lock(&listener->mutex);
  uint32_t generation = listener->generation;
  pthread_mutex_unlock(&listener->mutex);
  return generation;
}

void wp_ctl_listener_reply(wp_ctl_listener_t *listener, uint32_t generation,
                           const wp_ctl_response_t *response) {
  pthread_mutex_lock(&listener->mutex);
  if (listener->has_pending && listener->generation == generation) {
    listener->pending_reply = *response;
    listener->has_reply = true;
    pthread_cond_broadcast(&listener->cond);
  }
  pthread_mutex_unlock(&listener->mutex);
}
