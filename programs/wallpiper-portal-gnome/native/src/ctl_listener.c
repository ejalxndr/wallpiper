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

#include "ctl_listener.h"
#include "actor_stacking.h"
#include "capture_listener.h"
#include "error.h"
#include "monitor_geometry.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-unix.h>
#include <graphene.h>
#include <meta/meta-cursor-tracker.h>

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
  guint8 *pixels;
  guint32 width;
  guint32 height;
  gchar *path;
  int client_fd;
} wp_gnome_capture_job_t;

static void *wp_gnome_capture_encode_and_reply(void *arg) {
  wp_gnome_capture_job_t *job = arg;

  int rowstride = (int)job->width * 4;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
      job->pixels, GDK_COLORSPACE_RGB, TRUE, 8, (int)job->width,
      (int)job->height, rowstride, (GdkPixbufDestroyNotify)g_free, NULL);

  GError *save_error = NULL;
  gboolean saved = gdk_pixbuf_save(pixbuf, job->path, "png", &save_error, NULL);
  g_object_unref(pixbuf);

  gchar *response;
  if (saved) {
    response = g_strdup("OK\n");
  } else {
    response = g_strdup_printf("ERR %s\n", save_error ? save_error->message
                                                      : "failed to write PNG");
  }
  g_clear_error(&save_error);

  send(job->client_fd, response, strlen(response), 0);
  g_free(response);
  close(job->client_fd);

  g_free(job->path);
  g_free(job);
  return NULL;
}

static gboolean on_ctl_socket_connectable(gint fd, GIOCondition condition,
                                          gpointer user_data) {
  WallpiperPortalState *state = user_data;

  if (condition & (G_IO_ERR | G_IO_HUP)) {
    g_warning("wallpiper-gnome: ctl socket error/hangup");
    return G_SOURCE_REMOVE;
  }

  int client_fd = accept(fd, NULL, NULL);
  if (client_fd < 0)
    return G_SOURCE_CONTINUE;

  char buf[512];
  size_t total = 0;
  while (total + 1 < sizeof(buf)) {
    char c;
    ssize_t n = recv(client_fd, &c, 1, 0);
    if (n <= 0) {
      break;
    }
    buf[total++] = c;
    if (c == '\n') {
      break;
    }
  }
  buf[total] = '\0';
  if (total == 0) {
    close(client_fd);
    return G_SOURCE_CONTINUE;
  }

  const gchar *line = g_strstrip(buf);
  g_message("wallpiper-gnome: ctl request: %s", line);
  gchar *response;

  if (g_strcmp0(line, "GEOMETRY") == 0) {
    // gchar *json = wallpiper_monitor_geometry_to_json(&state->geometry);
    // response = g_strdup_printf("GEOMETRY %s\n", json);
    // g_free(json);
    response = g_strdup("ERR geometry detection disabled\n");
  } else if (g_strcmp0(line, "DETACH") == 0) {
    wallpiper_capture_listener_detach(state);
    response = g_strdup("OK\n");
  } else if (g_strcmp0(line, "DEBUG_ON") == 0) {
    state->debug_enabled = TRUE;
    g_message("wallpiper-gnome: debug enabled");
    wallpiper_actor_stacking_dump_children(state->parent, "debug-on");
    {
      for (int ch = 0; ch < WP_MAX_CAPTURE_CHANNELS; ch++) {
        ClutterActor *actor = state->channels[ch].display_actor;
        if (!actor)
          continue;
        guint idx = 0;
        gboolean found = FALSE;
        for (ClutterActor *child = clutter_actor_get_first_child(state->parent);
             child != NULL;
             child = clutter_actor_get_next_sibling(child), idx++) {
          if (child == actor) {
            g_message("wallpiper-gnome: [debug-on] channel %d display_actor "
                      "is at index %u",
                      ch, idx);
            found = TRUE;
            break;
          }
        }
        if (!found)
          g_message("wallpiper-gnome: [debug-on] channel %d display_actor "
                    "NOT FOUND among window_group children!",
                    ch);
      }
    }
    response = g_strdup("OK\n");
  } else if (g_strcmp0(line, "DEBUG_OFF") == 0) {
    state->debug_enabled = FALSE;
    g_message("wallpiper-gnome: debug disabled");
    response = g_strdup("OK\n");
  } else if (g_strcmp0(line, "CURSOR_POS") == 0) {
    MetaCursorTracker *tracker =
        meta_backend_get_cursor_tracker(state->backend);
    graphene_point_t point = GRAPHENE_POINT_INIT(0.f, 0.f);
    if (tracker)
      meta_cursor_tracker_get_pointer(tracker, &point, NULL);
    response =
        g_strdup_printf("CURSOR_POS %d %d\n", (int)point.x, (int)point.y);
  } else if (g_strcmp0(line, "PING") == 0) {
    response = g_strdup("OK\n");
  } else if (g_str_has_prefix(line, "CAPTURE ")) {
    gchar **parts = g_strsplit(line + strlen("CAPTURE "), " ", 2);
    gchar *channel_end = NULL;
    guint64 parsed_channel =
        parts[0] ? g_ascii_strtoull(parts[0], &channel_end, 10) : 0;
    gboolean channel_valid = parts[0] && channel_end != parts[0] &&
                             *channel_end == '\0' &&
                             parsed_channel <= G_MAXUINT32;
    guint32 channel = channel_valid ? (guint32)parsed_channel : 0;
    const gchar *path = channel_valid && parts[1] ? parts[1] : NULL;
    if (!path) {
      response = g_strdup("ERR malformed capture request\n");
      g_strfreev(parts);
    } else {
      guint8 *pixels = NULL;
      guint32 width = 0, height = 0;
      GError *capture_error = NULL;
      if (!wallpiper_capture_listener_readback(state, channel, &pixels, &width,
                                               &height, &capture_error)) {
        response =
            g_strdup_printf("ERR %s\n", capture_error ? capture_error->message
                                                      : "capture failed");
        g_clear_error(&capture_error);
        g_strfreev(parts);
      } else {
        wp_gnome_capture_job_t *job = g_new0(wp_gnome_capture_job_t, 1);
        job->pixels = pixels;
        job->width = width;
        job->height = height;
        job->path = g_strdup(path);
        job->client_fd = client_fd;
        g_strfreev(parts);

        pthread_t thread;
        if (pthread_create(&thread, NULL, wp_gnome_capture_encode_and_reply,
                           job) != 0) {
          g_free(pixels);
          g_free(job->path);
          g_free(job);
          response = g_strdup("ERR failed to spawn capture encode thread\n");
        } else {
          pthread_detach(thread);
          return G_SOURCE_CONTINUE;
        }
      }
    }
  } else {
    response = g_strdup_printf("ERR unrecognized command\n");
  }

  send(client_fd, response, strlen(response), 0);
  g_free(response);
  close(client_fd);

  return G_SOURCE_CONTINUE;
}

gboolean wallpiper_ctl_listener_start(WallpiperPortalState *state,
                                      GError **error) {
  int ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (ctl_fd < 0) {
    g_set_error(error, WALLPIPER_ERROR, 0, "ctl socket() failed: %s",
                g_strerror(errno));
    return FALSE;
  }

  unlink(WALLPIPER_CTL_SOCKET_PATH);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  g_strlcpy(addr.sun_path, WALLPIPER_CTL_SOCKET_PATH, sizeof(addr.sun_path));
  if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(ctl_fd, 4) != 0) {
    g_set_error(error, WALLPIPER_ERROR, 0, "bind/listen(%s) failed: %s",
                WALLPIPER_CTL_SOCKET_PATH, g_strerror(errno));
    close(ctl_fd);
    return FALSE;
  }

  state->ctl_socket_fd = ctl_fd;
  state->ctl_source_id =
      g_unix_fd_add(ctl_fd, G_IO_IN, on_ctl_socket_connectable, state);

  return TRUE;
}

void wallpiper_ctl_listener_stop(WallpiperPortalState *state) {
  g_source_remove(state->ctl_source_id);
  close(state->ctl_socket_fd);
  unlink(WALLPIPER_CTL_SOCKET_PATH);
}
