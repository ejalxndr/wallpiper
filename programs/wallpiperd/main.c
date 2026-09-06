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

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cleanup.h"
#include "commands.h"
#include "config.h"
#include "portal.h"
#include "renderer.h"
#include "signals.h"
#include "tray.h"
#include "vk_layer.h"

#include "wallpiper/daemon_ctl_protocol.h"
#include "wallpiper/fsutil.h"
#include "wallpiper/protocol.h"
#include "wallpiper/steam_paths.h"

static bool daemon_command_handler(const char *const *args, size_t arg_count,
                                   char *err_out, size_t err_out_len) {
  return wp_commands_dispatch(args, arg_count, err_out, err_out_len);
}

static void *readiness_wait_thread_main(void *arg) {
  (void)arg;
  wp_portal_wait_ready();
  wp_renderer_swap();
  return NULL;
}

static void *daemon_ctl_listener_thread_main(void *arg) {
  (void)arg;

  char path[512];
  if (!wp_daemon_ctl_socket_path(path, sizeof(path))) {
    printf("[ctl] failed to resolve daemon ctl socket path\n");
    return NULL;
  }
  wp_mkdir_p_parent(path);
  unlink(path);

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    printf("[ctl] failed to create socket\n");
    return NULL;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t path_len = strlen(path);
  if (path_len >= sizeof(addr.sun_path)) {
    printf("[ctl] socket path too long: %s\n", path);
    close(sock);
    return NULL;
  }
  memcpy(addr.sun_path, path, path_len);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    printf("[ctl] failed to bind %s\n", path);
    close(sock);
    return NULL;
  }
  if (listen(sock, 8) < 0) {
    printf("[ctl] failed to listen on %s\n", path);
    close(sock);
    return NULL;
  }
  printf("[ctl] listening on %s\n", path);

  for (;;) {
    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
      continue;
    }

    char line[1024];
    size_t total = 0;
    while (total + 1 < sizeof(line)) {
      char c;
      ssize_t n = read(conn, &c, 1);
      if (n <= 0) {
        break;
      }
      line[total++] = c;
      if (c == '\n') {
        break;
      }
    }
    line[total] = '\0';
    while (total > 0 && (line[total - 1] == '\n' || line[total - 1] == '\r')) {
      line[--total] = '\0';
    }

    char args_storage[1024];
    snprintf(args_storage, sizeof(args_storage), "%s", line);
    const char *argv_ptrs[64];
    size_t argc = 0;

    if (strncmp(args_storage, "capture ", 8) == 0) {
      char *p = args_storage + 8;
      while (*p == ' ' || *p == '\t') {
        p++;
      }
      char *monitor_tok = p;
      while (*p && *p != ' ' && *p != '\t') {
        p++;
      }
      if (p != monitor_tok && *p) {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') {
          p++;
        }
        if (*p) {
          argv_ptrs[argc++] = "capture";
          argv_ptrs[argc++] = monitor_tok;
          argv_ptrs[argc++] = p;
        }
      }
    } else {
      char *saveptr = NULL;
      char *tok = strtok_r(args_storage, " \t\r\n", &saveptr);
      while (tok && argc < 64) {
        argv_ptrs[argc++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &saveptr);
      }
    }

    char resp[512];
    char err[400];
    bool ok;
    if (argc == 0) {
      ok = false;
      snprintf(err, sizeof(err), "empty command");
    } else {
      ok = daemon_command_handler(argv_ptrs, argc, err, sizeof(err));
    }

    if (ok) {
      wp_daemon_ctl_encode_ok(resp, sizeof(resp));
    } else {
      wp_daemon_ctl_encode_err(err, resp, sizeof(resp));
    }
    write(conn, resp, strlen(resp));
    close(conn);
  }

  return NULL;
}

static void print_help(void) {
  printf(
      "usage: $ WALLPIPER_PORTAL=... WALLPIPER_... wallpiperd\n"
      "\n"
      "  WALLPIPER_PORTAL (*)\n"
      "      brief:   Which portal to use: hyprland, sway, cosmic, i3, gnome, "
      "kde.\n"
      "\n"
      "  WALLPIPER_STEAM_ROOT\n"
      "      default: auto-detected (~/.local/share/Steam, ~/.steam/steam,\n"
      "               ~/.steam/root, or the Flatpak path)\n"
      "      brief:   Your Steam library root.\n"
      "\n"
      "  WALLPIPER_PROTON_BIN\n"
      "      default: auto-detected under compatibilitytools.d/*/proton or\n"
      "               steamapps/common/Proton */proton\n"
      "      brief:   Path to the proton binary to run Wallpaper Engine with.\n"
      "\n"
      "  WALLPIPER_WE_EXE\n"
      "      default: $WALLPIPER_STEAM_ROOT/steamapps/common/\n"
      "               wallpaper_engine/wallpaper64.exe\n"
      "      brief:   Path to Wallpaper Engine's executable.\n"
      "\n"
      "  WALLPIPER_TEMP_DIR\n"
      "      default: /tmp/wallpiper\n"
      "      brief:   Directory used for ephemeral, session-scoped files,\n"
      "               control sockets, the Vulkan capture layer's search "
      "path,\n"
      "               tracked renderer PIDs, etc.\n"
      "\n"
      "  WALLPIPER_RUNTIME_DIR\n"
      "      default: $XDG_STATE_HOME/wallpiper, or ~/.local/state/wallpiper\n"
      "      brief:   Directory used for state that should persist across "
      "reboots.\n"
      "\n"
      "  WALLPIPER_CAPTURE_RENDER_NODE\n"
      "      default: auto-detected from the active portal's DRM render "
      "node\n"
      "      brief:   Which GPU (major:minor, e.g. 226:1) Wallpaper Engine's\n"
      "               renderer should use. Normally set automatically to "
      "match\n"
      "               whichever GPU the compositor/portal is bound to, so "
      "dmabuf\n"
      "               capture doesn't fail cross-GPU on hybrid/multi-GPU "
      "systems.\n"
      "               Set this yourself to override auto-detection.\n"
      "\n"
      "  WALLPIPER_WE_UI_SCALE_FACTOR\n"
      "      default: unset (no scaling override)\n"
      "      brief:   Forces N scale factor on Wallpaper Engine's\n"
      "               properties-panel process. Not auto-detected.\n"
      "\n"
      "  WALLPIPER_TRAY_OPTS\n"
      "      default: native\n"
      "      brief:   Controls how the Wallpaper Engine tray icon is "
      "translated.\n"
      "      options: [native, notray, passthrough]\n"
      "              native: over org.kde.StatusNotifierItem/dbusmenu. "
      "Supports all portals.\n"
      "              notray: no tray rendered at all\n"
      "         passthrough: pushes a raw legacy XEmbed tray icon, which "
      "only\n"
      "                      appears if your desktop runs a legacy tray "
      "host.\n"
      "\n"
      "Use `wallpiperctl` to control a running daemon.\n");
}

static void run(void) {
  printf("wallpiperd starting\n");

  wp_ignore_sigpipe();
  wp_reap_children_forever();
  wp_install_shutdown_handler(wp_cleanup);

  wp_write_vk_layer_manifest();
  if (!wp_tray_notray() && !wp_tray_passthrough()) {
    wp_tray_spawn();
  }

  char portal_name[64];
  char err[256];
  if (!wp_portal_name(portal_name, sizeof(portal_name), err, sizeof(err))) {
    fprintf(stderr, "%s\n", err);
    exit(1);
  }
  printf("using portal: %s\n", portal_name);

  wp_portal_strategy_t strategy;
  wp_portal_spawn_strategy(portal_name, &strategy);
  wp_portal_spawn(&strategy);

  bool patient = strategy.kind == WP_PORTAL_EXTERNALLY_MANAGED;
  wp_portal_spawn_readiness_watcher(portal_name, patient);

  pthread_t readiness_thread;
  pthread_create(&readiness_thread, NULL, readiness_wait_thread_main, NULL);
  pthread_detach(readiness_thread);

  pthread_t ctl_thread;
  pthread_create(&ctl_thread, NULL, daemon_ctl_listener_thread_main, NULL);
  pthread_detach(ctl_thread);

  printf("wallpiperd ready\n");
  for (;;) {
    sleep(3600);
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc > 1) {
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
      print_help();
      return 0;
    }
    fprintf(stderr, "wallpiperd takes no arguments; use `wallpiperctl` "
                    "to control a running daemon\n");
    return 1;
  }

  run();
  return 0;
}
