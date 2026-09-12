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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "wallpiper/daemon_ctl_protocol.h"
#include "wallpiper/steam_paths.h"
#include "we_control.h"

static bool resolve_absolute_path(const char *path, char *out,
                                  size_t out_len) {
  if (path[0] == '/') {
    return snprintf(out, out_len, "%s", path) < (int)out_len;
  }
  char cwd[768];
  if (!getcwd(cwd, sizeof(cwd))) {
    return false;
  }
  return snprintf(out, out_len, "%s/%s", cwd, path) < (int)out_len;
}

static const char *const DAEMON_COMMANDS[] = {
    "debug-on",
    "debug-off",
};
#define DAEMON_COMMAND_COUNT                                                   \
  (sizeof(DAEMON_COMMANDS) / sizeof(DAEMON_COMMANDS[0]))

typedef struct {
  const char *alias;
  const char *control_verb;
} we_verb_map_t;

static const we_verb_map_t WE_VERBS[] = {
    {"pause", "pause"},
    {"play", "play"},
    {"stop", "stop"},
    {"mute", "mute"},
    {"unmute", "unmute"},
    {"next", "nextWallpaper"},
    {"prev", "previousWallpaper"},
    {"reset", "resetWallpaper"},
};
#define WE_VERB_COUNT (sizeof(WE_VERBS) / sizeof(WE_VERBS[0]))

static bool is_daemon_command(const char *cmd) {
  for (size_t i = 0; i < DAEMON_COMMAND_COUNT; i++) {
    if (strcmp(cmd, DAEMON_COMMANDS[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool run_we_command(const char *cmd, int argc, char **argv, char *err,
                           size_t err_len) {
  if (strcmp(cmd, "list") == 0) {
    return wp_we_list_wallpapers(err, err_len);
  }
  if (strcmp(cmd, "set") == 0) {
    if (argc < 3) {
      snprintf(err, err_len,
               "usage: wallpiperctl set <path|workshop-id> [monitor]");
      return false;
    }
    const char *monitor = argc > 3 ? argv[3] : NULL;
    return wp_we_set_wallpaper(argv[2], monitor, err, err_len);
  }
  if (strcmp(cmd, "volume") == 0) {
    if (argc < 3) {
      snprintf(err, err_len, "usage: wallpiperctl volume <0-100>");
      return false;
    }
    const char *control_args[] = {"volume", "-value", argv[2]};
    return wp_we_send_control(control_args, 3, err, err_len);
  }
  if (strcmp(cmd, "prop") == 0) {
    if (argc < 5) {
      snprintf(err, err_len,
               "usage: wallpiperctl prop <path|workshop-id> <name> <value> "
               "[monitor]");
      return false;
    }
    const char *monitor = argc > 5 ? argv[5] : NULL;
    return wp_we_set_property(argv[2], argv[3], argv[4], monitor, err, err_len);
  }

  for (size_t i = 0; i < WE_VERB_COUNT; i++) {
    if (strcmp(cmd, WE_VERBS[i].alias) == 0) {
      const char *control_args[] = {WE_VERBS[i].control_verb};
      return wp_we_send_control(control_args, 1, err, err_len);
    }
  }

  snprintf(err, err_len, "unknown command: %s", cmd);
  return false;
}

static bool is_we_command(const char *cmd) {
  if (strcmp(cmd, "list") == 0 || strcmp(cmd, "set") == 0 ||
      strcmp(cmd, "volume") == 0 || strcmp(cmd, "prop") == 0) {
    return true;
  }
  for (size_t i = 0; i < WE_VERB_COUNT; i++) {
    if (strcmp(cmd, WE_VERBS[i].alias) == 0) {
      return true;
    }
  }
  return false;
}

static void print_usage(void) {
  fprintf(stderr,
          "usage: wallpiperctl <command>\n"
          "\n"
          "daemon commands (require a running wallpiperd):\n"
          "  debug-on | debug-off\n"
          "  capture <channel; int; 0-indexed, not necessarily a monitor "
          "index> <path>\n"
          "\n"
          "wallpaper engine commands:\n"
          "  pause | play   | stop\n"
          "  next  | prev   | reset\n"
          "  mute  | unmute | volume <0-100>\n"
          "  set <path|workshop-id> [monitor; int; 0-indexed]\n"
          "  prop <path|workshop-id> <name> <value> [monitor; int; 0-indexed]\n"
          "  list\n"
          "\n"
          "standalone commands:\n"
          "  check-config\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  const char *cmd = argv[1];

  if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
    print_usage();
    return 0;
  }

  char err[512];
  bool ok;

  if (strcmp(cmd, "check-config") == 0) {
    wp_describe();
    ok = true;
  } else if (is_we_command(cmd)) {
    ok = run_we_command(cmd, argc, argv, err, sizeof(err));
  } else if (strcmp(cmd, "capture") == 0) {
    if (argc < 4) {
      snprintf(err, sizeof(err),
               "usage: wallpiperctl capture <channel> <path>");
      ok = false;
    } else {
      char abs_path[1024];
      if (!resolve_absolute_path(argv[3], abs_path, sizeof(abs_path))) {
        snprintf(err, sizeof(err), "path too long: %s", argv[3]);
        ok = false;
      } else {
        const char *capture_args[] = {"capture", argv[2], abs_path};
        ok = wp_send_daemon_command(capture_args, 3, err, sizeof(err));
        if (ok) {
          printf("wrote %s\n", abs_path);
        }
      }
    }
  } else if (is_daemon_command(cmd)) {
    ok = wp_send_daemon_command((const char *const *)(argv + 1),
                                (size_t)(argc - 1), err, sizeof(err));
  } else {
    fprintf(stderr, "wallpiperctl: unknown command: %s\n\n", cmd);
    print_usage();
    return 1;
  }

  if (!ok) {
    fprintf(stderr, "wallpiperctl: %s\n", err);
    return 1;
  }
  return 0;
}
