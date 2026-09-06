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

#include "portal.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "wallpiper/ctl_protocol.h"
#include "wallpiper/steam_paths.h"

static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_display_pid = -1;
static bool g_has_monitor = false;
static wp_monitor_geometry_t g_monitor;
static bool g_has_render_node = false;
static bool g_render_node_attempted = false;
static uint32_t g_render_node_major = 0;
static uint32_t g_render_node_minor = 0;

#define WP_RENDER_NODE_ATTEMPTS 10
#define WP_RENDER_NODE_RETRY_MS 150

static void fetch_render_node(const char *name) {
  for (int attempt = 0; attempt < WP_RENDER_NODE_ATTEMPTS; attempt++) {
    wp_ctl_response_t node_resp;
    if (wp_send_ctl_request(name, WP_CTL_REQUEST_RENDER_NODE, &node_resp) &&
        node_resp.tag == WP_CTL_RESPONSE_RENDER_NODE) {
      printf("%s portal render node: %u:%u\n", name,
             node_resp.render_node_major, node_resp.render_node_minor);
      pthread_mutex_lock(&g_state_mutex);
      g_render_node_major = node_resp.render_node_major;
      g_render_node_minor = node_resp.render_node_minor;
      g_has_render_node = true;
      g_render_node_attempted = true;
      pthread_mutex_unlock(&g_state_mutex);
      return;
    }
    struct timespec ts = {.tv_sec = 0,
                          .tv_nsec = WP_RENDER_NODE_RETRY_MS * 1000000L};
    nanosleep(&ts, NULL);
  }
  printf("%s portal did not report a render node\n", name);
  pthread_mutex_lock(&g_state_mutex);
  g_render_node_attempted = true;
  pthread_mutex_unlock(&g_state_mutex);
}

void wp_portal_spawn_strategy(const char *name, wp_portal_strategy_t *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "%s", name);

  if (strcmp(name, "kde") == 0 || strcmp(name, "gnome") == 0) {
    out->kind = WP_PORTAL_EXTERNALLY_MANAGED;
    return;
  }

  out->kind = WP_PORTAL_SPAWN;
  char install_dir[1024];
  if (wp_install_dir(install_dir, sizeof(install_dir))) {
    snprintf(out->binary, sizeof(out->binary), "%.500s/wallpiper-portal-%.63s",
             install_dir, name);
  }
}

void wp_portal_spawn(const wp_portal_strategy_t *strategy) {
  if (strategy->kind == WP_PORTAL_EXTERNALLY_MANAGED) {
    printf("portal %s is externally managed, not spawning: waiting for its ctl "
           "socket to appear\n",
           strategy->name);
    return;
  }

  char logpath[512];
  snprintf(logpath, sizeof(logpath), "/tmp/wallpiperd-portal-%s.log",
           strategy->name);
  int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (logfd < 0) {
    printf("failed to create portal logfile\n");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    printf("failed to spawn portal (%s): fork failed\n", strategy->binary);
    close(logfd);
    return;
  }

  if (pid == 0) {
    dup2(logfd, STDOUT_FILENO);
    dup2(logfd, STDERR_FILENO);
    close(logfd);
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) {
      raise(SIGKILL);
    }
    execl(strategy->binary, strategy->binary, (char *)NULL);
    _exit(127);
  }

  close(logfd);
  printf("spawned portal (%s) pid=%d\n", strategy->binary, (int)pid);
  pthread_mutex_lock(&g_state_mutex);
  g_display_pid = (int)pid;
  pthread_mutex_unlock(&g_state_mutex);
}

typedef struct {
  char name[64];
  bool patient;
} readiness_watcher_args_t;

static void *readiness_watcher_thread(void *arg) {
  readiness_watcher_args_t *args = arg;
  char name[64];
  snprintf(name, sizeof(name), "%s", args->name);
  bool patient = args->patient;
  free(args);

  bool use_geometry = strcmp(name, "kde") == 0;

  long interval_ms = patient ? 2000 : 300;
  unsigned attempt = 0;

  for (;;) {
    attempt++;
    wp_ctl_response_t resp;
    bool ready;

    if (use_geometry) {
      ready = wp_send_ctl_request(name, WP_CTL_REQUEST_GEOMETRY, &resp) &&
              resp.tag == WP_CTL_RESPONSE_GEOMETRY;
    } else {
      ready = wp_send_ctl_request(name, WP_CTL_REQUEST_PING, &resp);
    }

    if (ready) {
      printf("%s portal ready...\n", name);
      pthread_mutex_lock(&g_state_mutex);
      if (use_geometry) {
        g_monitor = resp.geometry;
      }
      g_has_monitor = true;
      pthread_mutex_unlock(&g_state_mutex);

      fetch_render_node(name);
      return NULL;
    }

    if (!patient && attempt >= 10) {
      printf("portal %s never answered on its ctl socket, proceeding anyway\n",
             name);
      pthread_mutex_lock(&g_state_mutex);
      g_has_monitor = true;
      pthread_mutex_unlock(&g_state_mutex);
      fetch_render_node(name);
      return NULL;
    }

    struct timespec ts = {.tv_sec = interval_ms / 1000,
                          .tv_nsec = (interval_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }
}

void wp_portal_spawn_readiness_watcher(const char *name, bool patient) {
  readiness_watcher_args_t *args = malloc(sizeof(*args));
  if (!args) {
    return;
  }
  snprintf(args->name, sizeof(args->name), "%s", name);
  args->patient = patient;

  pthread_t thread;
  if (pthread_create(&thread, NULL, readiness_watcher_thread, args) != 0) {
    free(args);
    return;
  }
  pthread_detach(thread);
}

void wp_portal_wait_ready(void) {
  for (;;) {
    pthread_mutex_lock(&g_state_mutex);
    bool has = g_has_monitor && g_render_node_attempted;
    pthread_mutex_unlock(&g_state_mutex);
    if (has) {
      return;
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000000L};
    nanosleep(&ts, NULL);
  }
}

bool wp_portal_current_monitor(wp_monitor_geometry_t *out) {
  pthread_mutex_lock(&g_state_mutex);
  bool has = g_has_monitor;
  if (has) {
    *out = g_monitor;
  }
  pthread_mutex_unlock(&g_state_mutex);
  return has;
}

bool wp_portal_current_render_node(uint32_t *major, uint32_t *minor) {
  pthread_mutex_lock(&g_state_mutex);
  bool has = g_has_render_node;
  if (has) {
    *major = g_render_node_major;
    *minor = g_render_node_minor;
  }
  pthread_mutex_unlock(&g_state_mutex);
  return has;
}

bool wp_portal_query_monitor_once(const char *name,
                                  wp_monitor_geometry_t *out) {
  wp_ctl_response_t resp;
  if (wp_send_ctl_request(name, WP_CTL_REQUEST_GEOMETRY, &resp) &&
      resp.tag == WP_CTL_RESPONSE_GEOMETRY) {
    *out = resp.geometry;
    return true;
  }
  return false;
}

bool wp_portal_detach_display(void) {
  char portal_name[64];
  char err[256];
  if (!wp_portal_name(portal_name, sizeof(portal_name), err, sizeof(err))) {
    return false;
  }
  wp_ctl_response_t resp;
  return wp_send_ctl_request(portal_name, WP_CTL_REQUEST_DETACH, &resp) &&
         resp.tag == WP_CTL_RESPONSE_OK;
}

void wp_portal_set_debug_overlay(bool enabled) {
  char portal_name[64];
  char err[256];
  if (!wp_portal_name(portal_name, sizeof(portal_name), err, sizeof(err))) {
    return;
  }
  wp_ctl_response_t resp;
  bool ok = wp_send_ctl_request(portal_name,
                                enabled ? WP_CTL_REQUEST_DEBUG_ON
                                        : WP_CTL_REQUEST_DEBUG_OFF,
                                &resp) &&
            resp.tag == WP_CTL_RESPONSE_OK;
  printf("debug overlay %s -> %s\n", enabled ? "on" : "off",
         ok ? "ok" : "failed");
}

bool wp_portal_capture_frame(uint32_t channel, const char *path, char *err,
                             size_t err_len) {
  char portal_name[64];
  if (!wp_portal_name(portal_name, sizeof(portal_name), err, err_len)) {
    return false;
  }
  wp_ctl_response_t resp;
  if (!wp_send_ctl_capture_request(portal_name, channel, path, &resp)) {
    snprintf(err, err_len, "no response from portal %s", portal_name);
    return false;
  }
  if (resp.tag == WP_CTL_RESPONSE_OK) {
    return true;
  }
  if (resp.tag == WP_CTL_RESPONSE_ERR) {
    snprintf(err, err_len, "%s", resp.err);
  } else {
    snprintf(err, err_len, "unexpected response from portal");
  }
  return false;
}

bool wp_portal_take_display_pid(int *out_pid) {
  pthread_mutex_lock(&g_state_mutex);
  bool has = g_display_pid >= 0;
  if (has) {
    *out_pid = g_display_pid;
    g_display_pid = -1;
  }
  pthread_mutex_unlock(&g_state_mutex);
  return has;
}
