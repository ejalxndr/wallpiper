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
 *
 * Native playback-condition forwarding for the KDE portal.
 *
 * Wallpaper Engine decides "pause when other application is
 * focused/maximized/fullscreen" from per-monitor bitmasks inside
 * wallpaper64.exe:
 *
 *   RVA 0x4e5300 : focus mask      (bit N = monitor N)
 *   RVA 0x4e5304 : maximized mask  (bit N = monitor N)
 *   RVA 0x4e5308 : fullscreen mask (bit N = monitor N)
 *
 * It also reads two globals for the audio condition and one system-flag bit
 * for the sleep condition:
 *
 *   RVA 0x4e6198 : audio activity detected (byte)
 *   RVA 0x4e52e4 : system flags, bit 0x20 = screen off
 *
 * Those values are recomputed from Win32 signals inside the playback
 * evaluator, which cannot see native Plasma windows, PipeWire streams or
 * Plasma's screensaver state.  We hook the internal detector at RVA 0x3d050,
 * let it run, then OR/inject the native values before the evaluator reads
 * them.  No new wallpiperctl option is added: the user's existing
 * playbackfocus/playbackaudio/playbacksleep settings decide the action.
 *
 * The file carries screen rects, not mask bits: capture channels follow WE's
 * swapchain creation order and are not stable across launches, while the mask
 * bits use WE's own monitor index.  We map each rect to a monitor index with
 * EnumDisplayMonitors in this process, the same enumeration WE uses.  The
 * legacy 8-byte (channel-indexed) file is still accepted as a fallback.
 *
 * All RVAs are from wallpaper64.exe, PE image base 0x140000000.  A byte-pattern
 * guard is checked before installing so a future WE update fails safe.
 */

#include "native_window.h"
#include "string.h"
#include "util.h"
#include "wallpiper/native_window_state.h"

/* cursor.c compiles the snare implementation into this DLL; here we only
 * reference the extern declarations. */
#define SNARE_STATIC
#include "libsnare.h"

/* --- offsets in wallpaper64.exe (validated against the installed binary) ---
 */
#define RVA_DETECT_CALLBACK 0x3d050u /* FUN_14003d050: recompute window state  \
                                      */
#define RVA_FOCUS_MASK 0x4e5300u
#define RVA_MAXIMIZED_MASK 0x4e5304u
#define RVA_FULLSCREEN_MASK 0x4e5308u
#define RVA_SYSTEM_FLAGS 0x4e52e4u
#define RVA_AUDIO_ACTIVITY 0x4e6198u

#define SYSTEM_FLAG_SCREEN_OFF 0x20u

static const unsigned char kDetectPrologue[8] = {
    0x40, 0x56,            /* push rsi */
    0x41, 0x54,            /* push r12 */
    0x48, 0x83, 0xec, 0x58 /* sub rsp, 0x58 */
};

typedef void(WINAPI *detect_callback_t)(void *state);
typedef UINT_PTR(WINAPI *set_timer_t)(HWND, UINT_PTR, UINT, void *);

/*
 * WE only re-evaluates window state from its 0x68 WM_TIMER, whose interval is
 * the user setting `windowupdaterate` (clamped by WE to >=50 ms).  On this
 * setup that interval is ~1000 ms, which is what makes pausing feel delayed.
 * Clamp just that timer to 100 ms so the injected state is acted on promptly.
 */
#define WINDOW_UPDATE_TIMER_ID 0x68u
#define WINDOW_UPDATE_INTERVAL_MS 100u

static detect_callback_t real_detect_callback;
static set_timer_t real_set_timer;
static snare_inline_t g_set_timer_hook;
static volatile unsigned int g_fullscreen_mask;
static volatile unsigned int g_maximized_mask;
static volatile unsigned int g_focus_mask;
static volatile unsigned int g_audio_active;
static volatile unsigned int g_screen_off;
static volatile unsigned int g_hook_calls;
static unsigned int *g_fullscreen_addr;
static unsigned int *g_maximized_addr;
static unsigned int *g_focus_addr;
static unsigned int *g_system_flags_addr;
static unsigned char *g_audio_activity_addr;

/* Only the hook thread touches these transition guards. */
static unsigned int g_prev_audio_active;
static unsigned int g_prev_screen_off;

#define MAX_TRACKED_MONITORS 8u
static RECT g_monitor_rects[MAX_TRACKED_MONITORS];
static unsigned int g_monitor_count;
static unsigned int g_unmapped_screens;

/* Build "Z:<dir of cursor file>/wallpiper-native-window" from the cursor path
 * env var, so the state file follows WALLPIPER_TEMP_DIR like all other
 * wallpiper IPC files. */
static BOOL get_state_file_path(char *out, DWORD out_len) {
  char unix_path[MAX_PATH];
  DWORD len = GetEnvironmentVariableA("WALLPIPER_CURSOR_POS_FILE", unix_path,
                                      sizeof(unix_path));
  if (len == 0 || len >= sizeof(unix_path)) {
    return FALSE;
  }

  char *last_slash = NULL;
  for (char *p = unix_path; *p; ++p) {
    if (*p == '/') {
      last_slash = p;
    }
  }
  if (!last_slash) {
    return FALSE;
  }

  static const char name[] = "wallpiper-native-window";
  size_t dir_len = (size_t)(last_slash - unix_path);
  size_t needed =
      2 + dir_len + 1 + sizeof(name); /* "Z:" + dir + "/" + name + NUL */
  if (needed > (size_t)out_len) {
    return FALSE;
  }

  out[0] = 'Z';
  out[1] = ':';
  memcpy(out + 2, unix_path, dir_len);
  out[2 + dir_len] = '/';
  strcpy(out + 3 + dir_len, name);
  return TRUE;
}

static BOOL WINAPI monitor_enum_callback(HMONITOR monitor, HDC dc, RECT *rect,
                                         LPARAM data) {
  (void)monitor;
  (void)dc;
  (void)data;
  if (g_monitor_count < MAX_TRACKED_MONITORS) {
    g_monitor_rects[g_monitor_count++] = *rect;
  }
  return TRUE;
}

static void refresh_monitor_rects(void) {
  g_monitor_count = 0;
  EnumDisplayMonitors(NULL, NULL, monitor_enum_callback, 0);
}

/*
 * Translate the portal's native screen rects into WE's per-monitor bitmasks.
 * EnumDisplayMonitors order is the order WE itself uses for its monitor list;
 * capture channel order is not (it follows swapchain creation and varies per
 * launch).
 */
static void masks_from_state(const WpNativeWindowState *state,
                             unsigned int *fullscreenMask,
                             unsigned int *maximizedMask,
                             unsigned int *focusMask) {
  *fullscreenMask = 0;
  *maximizedMask = 0;
  *focusMask = 0;

  const int origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);

  unsigned int i;
  for (i = 0; i < state->count && i < WP_NATIVE_WINDOW_STATE_MAX_SCREENS; ++i) {
    const WpNativeWindowScreen *screen = &state->screens[i];
    const int x = screen->x + origin_x;
    const int y = screen->y + origin_y;
    unsigned int j;
    for (j = 0; j < g_monitor_count; ++j) {
      const RECT *rect = &g_monitor_rects[j];
      if (x == rect->left && y == rect->top &&
          screen->width == rect->right - rect->left &&
          screen->height == rect->bottom - rect->top) {
        if (j < 32) {
          if (screen->flags & WP_NATIVE_WINDOW_FLAG_FULLSCREEN) {
            *fullscreenMask |= 1u << j;
          }
          if (screen->flags & WP_NATIVE_WINDOW_FLAG_MAXIMIZED) {
            *maximizedMask |= 1u << j;
          }
          if (screen->flags & WP_NATIVE_WINDOW_FLAG_FOCUSED) {
            *focusMask |= 1u << j;
          }
        }
        break;
      }
    }
    if (j == g_monitor_count) {
      ++g_unmapped_screens;
    }
  }
}

static DWORD WINAPI state_poll_thread(LPVOID param) {
  (void)param;

  char path[MAX_PATH + 4];
  if (!get_state_file_path(path, sizeof(path))) {
    debug_log("native_window: WALLPIPER_CURSOR_POS_FILE not set");
    return 0;
  }

  refresh_monitor_rects();
  char monitor_msg[96];
  wsprintfA(monitor_msg, "native_window: enumerated %u monitors",
            g_monitor_count);
  debug_log(monitor_msg);

  debug_log("native_window: state poll thread started");
  unsigned int log_ticks = 0;

  for (;;) {
    Sleep(100);

    HANDLE h =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
      unsigned char payload[sizeof(WpNativeWindowState)];
      memset(payload, 0, sizeof(payload));
      DWORD read_bytes = 0;
      BOOL ok = ReadFile(h, payload, sizeof(payload), &read_bytes, NULL);
      CloseHandle(h);
      if (ok && read_bytes >= sizeof(unsigned int) * 2) {
        unsigned int magic = 0;
        if (read_bytes >= sizeof(unsigned int) * 3) {
          memcpy(&magic, payload + sizeof(unsigned int) * 2, sizeof(magic));
        }

        if (magic == WP_NATIVE_WINDOW_STATE_MAGIC &&
            read_bytes >= sizeof(WpNativeWindowState)) {
          WpNativeWindowState state;
          memcpy(&state, payload, sizeof(state));
          unsigned int fs = 0;
          unsigned int mx = 0;
          unsigned int focus = 0;
          masks_from_state(&state, &fs, &mx, &focus);
          g_fullscreen_mask = fs;
          g_maximized_mask = mx;
          g_focus_mask = focus;
          g_audio_active = state.audio_active ? 1u : 0u;
          g_screen_off = state.screen_off ? 1u : 0u;
        } else if (magic == WP_NATIVE_WINDOW_STATE_V1_MAGIC &&
                   read_bytes >= WP_NATIVE_WINDOW_STATE_V1_SIZE) {
          WpNativeWindowState state;
          memset(&state, 0, sizeof(state));
          memcpy(&state, payload, WP_NATIVE_WINDOW_STATE_V1_SIZE);
          unsigned int fs = 0;
          unsigned int mx = 0;
          unsigned int focus = 0;
          masks_from_state(&state, &fs, &mx, &focus);
          g_fullscreen_mask = fs;
          g_maximized_mask = mx;
          g_focus_mask = 0;
          g_audio_active = 0;
          g_screen_off = 0;
        } else {
          /* Legacy 8-byte, channel-indexed payload. */
          g_fullscreen_mask = ((const unsigned int *)payload)[0];
          g_maximized_mask = ((const unsigned int *)payload)[1];
          g_focus_mask = 0;
          g_audio_active = 0;
          g_screen_off = 0;
        }
      }
    }

    /* Every ~5 s, report that the hook is alive and which state it sees. */
    if (++log_ticks >= 50u) {
      log_ticks = 0;
      char msg[256];
      wsprintfA(msg,
                "native_window: hook_calls=%u monitors=%u unmapped=%u "
                "fs=%u mx=%u focus=%u audio=%u screen_off=%u",
                g_hook_calls, g_monitor_count, g_unmapped_screens,
                g_fullscreen_mask, g_maximized_mask, g_focus_mask,
                g_audio_active, g_screen_off);
      debug_log(msg);
    }
  }
}

/*
 * WE's playback evaluator reads the masks and globals immediately after the
 * hooked detector returns.  Injecting here means WE's own settings still
 * choose the action, and its own Win32 detections are preserved with OR.
 */
static void inject_native_playback_state(void) {
  unsigned int focus = g_focus_mask;
  if (focus && g_focus_addr) {
    *g_focus_addr |= focus;
  }

  unsigned int audio = g_audio_active;
  if (audio && g_audio_activity_addr) {
    *g_audio_activity_addr = 1;
  } else if (g_prev_audio_active && g_audio_activity_addr) {
    /* Only clear a transition we previously injected. */
    *g_audio_activity_addr = 0;
  }
  g_prev_audio_active = audio;

  unsigned int screen_off = g_screen_off;
  if (screen_off && g_system_flags_addr) {
    *g_system_flags_addr |= SYSTEM_FLAG_SCREEN_OFF;
  } else if (g_prev_screen_off && g_system_flags_addr) {
    /* Only clear a transition we previously injected. */
    *g_system_flags_addr &= ~SYSTEM_FLAG_SCREEN_OFF;
  }
  g_prev_screen_off = screen_off;
}

static void WINAPI fake_detect_callback(void *state) {
  if (real_detect_callback) {
    real_detect_callback(state);
  }

  g_hook_calls++;
  inject_native_playback_state();

  unsigned int fs = g_fullscreen_mask;
  unsigned int mx = g_maximized_mask;
  if (!fs && !mx) {
    return;
  }

  /* OR after the original has reset/recomputed the masks. */
  if (fs && g_fullscreen_addr) {
    *g_fullscreen_addr |= fs;
  }
  if (mx && g_maximized_addr) {
    *g_maximized_addr |= mx;
  }
}

static UINT_PTR WINAPI fake_set_timer(HWND hWnd, UINT_PTR nIDEvent,
                                      UINT uElapse, void *lpTimerFunc) {
  if (nIDEvent == WINDOW_UPDATE_TIMER_ID &&
      uElapse > WINDOW_UPDATE_INTERVAL_MS) {
    uElapse = WINDOW_UPDATE_INTERVAL_MS;
  }
  return real_set_timer ? real_set_timer(hWnd, nIDEvent, uElapse, lpTimerFunc)
                        : 0;
}

static void install_window_update_timer_hook(void) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    user32 = LoadLibraryW(L"user32.dll");
  }
  if (!user32) {
    debug_log("native_window: could not resolve user32.dll for SetTimer");
    return;
  }

  void *target = (void *)GetProcAddress(user32, "SetTimer");
  if (!target) {
    debug_log("native_window: GetProcAddress(SetTimer) failed");
    return;
  }

  snare_inline_t hook = snare_inline_new(target, (void *)fake_set_timer);
  if (!hook) {
    debug_log("native_window: snare_inline_new(SetTimer) failed");
    return;
  }

  if (snare_inline_install(hook) != 0) {
    debug_log("native_window: snare_inline_install(SetTimer) failed");
    snare_inline_free(hook);
    return;
  }

  real_set_timer = (set_timer_t)snare_inline_get_trampoline(hook);
  if (!real_set_timer) {
    debug_log("native_window: no SetTimer trampoline, removing hook");
    snare_inline_remove(hook);
    snare_inline_free(hook);
    return;
  }

  g_set_timer_hook = hook;
  debug_log("native_window: clamped window update timer to 100 ms");
}

void install_native_window_hook(void) {
  if (!running_as_wallpaper64()) {
    return;
  }

  HMODULE base = GetModuleHandleW(NULL);
  if (!base) {
    debug_log("native_window: no main module");
    return;
  }

  unsigned char *fn = (unsigned char *)base + RVA_DETECT_CALLBACK;
  for (unsigned int i = 0; i < sizeof(kDetectPrologue); ++i) {
    if (fn[i] != kDetectPrologue[i]) {
      debug_log("native_window: prologue mismatch, skipping (WE update?)");
      return;
    }
  }

  g_fullscreen_addr =
      (unsigned int *)((unsigned char *)base + RVA_FULLSCREEN_MASK);
  g_maximized_addr =
      (unsigned int *)((unsigned char *)base + RVA_MAXIMIZED_MASK);
  g_focus_addr = (unsigned int *)((unsigned char *)base + RVA_FOCUS_MASK);
  g_system_flags_addr =
      (unsigned int *)((unsigned char *)base + RVA_SYSTEM_FLAGS);
  g_audio_activity_addr =
      (unsigned char *)((unsigned char *)base + RVA_AUDIO_ACTIVITY);

  snare_inline_t hook = snare_inline_new(fn, (void *)fake_detect_callback);
  if (!hook) {
    debug_log("native_window: snare_inline_new failed");
    return;
  }

  if (snare_inline_install(hook) != 0) {
    debug_log("native_window: snare_inline_install failed");
    snare_inline_free(hook);
    return;
  }

  real_detect_callback = (detect_callback_t)snare_inline_get_trampoline(hook);
  if (!real_detect_callback) {
    debug_log("native_window: no trampoline, removing hook");
    snare_inline_remove(hook);
    return;
  }

  char msg[128];
  wsprintfA(msg, "native_window: hooked detector at %p (base %p)", (void *)fn,
            (void *)base);
  debug_log(msg);

  install_window_update_timer_hook();

  HANDLE thread = CreateThread(NULL, 0, state_poll_thread, NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
  } else {
    debug_log("native_window: failed to start state poll thread");
  }
}
