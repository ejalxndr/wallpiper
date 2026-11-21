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

#include "cursor.h"
#include "util.h"

#define SNARE_STATIC
#define SNARE_IMPLEMENTATION
#include "libsnare.h"

UINT64 g_tray_hwnd;

static DWORD WINAPI cursor_forward_thread(LPVOID param) {
  (void)param;
  debug_log("cursor_forward_thread: started");

  for (;;) {
    Sleep(16);

    HWND hwnd = (HWND)(ULONG_PTR)g_tray_hwnd;
    if (!hwnd) {
      continue;
    }

    POINT pt;
    if (!GetCursorPos(&pt)) {
      continue;
    }

    POINT client = pt;
    ScreenToClient(hwnd, &client);

    PostMessageW(hwnd, WM_MOUSEMOVE, 0,
                 MAKELPARAM((WORD)client.x, (WORD)client.y));
  }

  return 0;
}

static BOOL get_cursor_pos_file_path(char *out, DWORD out_len) {
  char unix_path[MAX_PATH];
  DWORD len = GetEnvironmentVariableA("WALLPIPER_CURSOR_POS_FILE", unix_path,
                                      sizeof(unix_path));
  if (len == 0 || len >= sizeof(unix_path)) {
    return FALSE;
  }
  int n = wsprintfA(out, "Z:%s", unix_path);
  return n > 0 && (DWORD)n < out_len;
}

static volatile LONGLONG g_forwarded_cursor;
static volatile LONG g_have_forwarded_cursor;

static void store_forwarded_cursor(INT32 x, INT32 y) {
  UINT64 packed = ((UINT64)(UINT32)x << 32) | (UINT32)y;
  InterlockedExchange64((LONGLONG volatile *)&g_forwarded_cursor,
                        (LONGLONG)packed);
  InterlockedExchange((LONG volatile *)&g_have_forwarded_cursor, 1);
}

static BOOL load_forwarded_cursor(LONG *x, LONG *y) {
  if (!InterlockedCompareExchange((LONG volatile *)&g_have_forwarded_cursor, 0,
                                  0)) {
    return FALSE;
  }
  UINT64 packed = (UINT64)InterlockedCompareExchange64(
      (LONGLONG volatile *)&g_forwarded_cursor, 0, 0);
  *x = (LONG)(INT32)(packed >> 32);
  *y = (LONG)(INT32)(packed & 0xFFFFFFFF);
  return TRUE;
}

typedef BOOL(WINAPI *GetCursorPos_t)(LPPOINT);
static GetCursorPos_t real_GetCursorPos;

static volatile unsigned int g_cursor_hook_calls;
static int g_wine_screen_origin_x = 0;
static int g_wine_screen_origin_y = 0;

static void log_cursor_hook(int outX, int outY, int fromWine,
                            int portalX, int portalY) {
  if (++g_cursor_hook_calls % 256u != 0u) {
    return;
  }
  char msg[160];
  wsprintfA(msg,
            "cursor_hook: calls=%u out=%d,%d source=%s portal=%d,%d origin=%d,%d",
            g_cursor_hook_calls, outX, outY, fromWine ? "wine" : "portal",
            portalX, portalY, g_wine_screen_origin_x,
            g_wine_screen_origin_y);
  debug_log(msg);
}

/*
 * Coordinate spaces (measured with GetSystemMetrics in the running Proton
 * prefix):
 *
 *   X11 global:  left monitor 0..1920, right/primary 1920..4480.
 *   Wine screen: primary at 0..2560, left monitor -1920..0,
 *                SM_XVIRTUALSCREEN = -1920, SM_YVIRTUALSCREEN = 0.
 *
 * Measured mapping: X11 (3865,1196) -> Wine GetCursorPos (1946,1197), i.e.
 *   wine = x11 + (SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN).
 *
 * Wallpaper Engine calls GetCursorPos, then ScreenToClient on the wallpaper
 * window (which lives at Wine (-1920,0) for the left monitor), then divides by
 * the 1920x1080 scene size.  Feeding raw X11 coords makes ScreenToClient add
 * 1920, pushing left-monitor client x to ~3336 (normalized x ~1.74), so the
 * mouse-linked control point lands far off-screen and never reacts.
 *
 * The portal supplies X11 global coordinates, so translate them into Wine's
 * screen space before handing them to GetCursorPos.  The real Wine cursor is
 * already in Wine space (but frozen on this Wayland session), so leave it
 * untouched as a fallback.
 */
static BOOL WINAPI fake_GetCursorPos(LPPOINT lpPoint) {
  if (!lpPoint) {
    return real_GetCursorPos ? real_GetCursorPos(lpPoint) : FALSE;
  }

  LONG x, y;
  if (load_forwarded_cursor(&x, &y)) {
    LONG portalX = x;
    LONG portalY = y;
    x += g_wine_screen_origin_x;
    y += g_wine_screen_origin_y;
    lpPoint->x = x;
    lpPoint->y = y;
    log_cursor_hook((int)x, (int)y, 0, (int)portalX, (int)portalY);
    return TRUE;
  }

  if (real_GetCursorPos && real_GetCursorPos(lpPoint)) {
    log_cursor_hook((int)lpPoint->x, (int)lpPoint->y, 1, 0, 0);
    return TRUE;
  }
  return FALSE;
}

static snare_inline_t g_get_cursor_pos_hook;

static void install_get_cursor_pos_hook(void) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    user32 = LoadLibraryW(L"user32.dll");
  }
  if (!user32) {
    debug_log("install_get_cursor_pos_hook: could not resolve user32.dll");
    return;
  }

  void *target = (void *)GetProcAddress(user32, "GetCursorPos");
  if (!target) {
    debug_log(
        "install_get_cursor_pos_hook: GetProcAddress(GetCursorPos) failed");
    return;
  }

  snare_inline_t hook = snare_inline_new(target, (void *)fake_GetCursorPos);
  if (!hook) {
    debug_log("install_get_cursor_pos_hook: snare_inline_new failed");
    return;
  }

  if (snare_inline_install(hook) != 0) {
    debug_log("install_get_cursor_pos_hook: snare_inline_install failed");
    snare_inline_free(hook);
    return;
  }

  real_GetCursorPos = (GetCursorPos_t)snare_inline_get_trampoline(hook);
  g_get_cursor_pos_hook = hook;
  debug_log("install_get_cursor_pos_hook: inline hook installed on "
            "user32.dll!GetCursorPos");
}

static DWORD WINAPI cursor_ipc_poll_thread(LPVOID param) {
  (void)param;

  char path[MAX_PATH + 2];
  if (!get_cursor_pos_file_path(path, sizeof(path))) {
    debug_log("cursor_ipc_poll_thread: WALLPIPER_CURSOR_POS_FILE not set");
    return 0;
  }

  debug_log("cursor_ipc_poll_thread: started");

  for (;;) {
    Sleep(16);

    HANDLE h =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }

    INT32 payload[2];
    DWORD read_bytes = 0;
    BOOL ok = ReadFile(h, payload, sizeof(payload), &read_bytes, NULL);
    CloseHandle(h);

    if (!ok || read_bytes != sizeof(payload)) {
      continue;
    }

    store_forwarded_cursor(payload[0], payload[1]);
  }

  return 0;
}

void install_cursor_hooks(void) {
  g_wine_screen_origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  g_wine_screen_origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  char origin_msg[96];
  wsprintfA(origin_msg, "cursor: wine screen origin %d,%d",
            g_wine_screen_origin_x, g_wine_screen_origin_y);
  debug_log(origin_msg);

  install_get_cursor_pos_hook();

  HANDLE cursor_thread =
      CreateThread(NULL, 0, cursor_forward_thread, NULL, 0, NULL);
  if (cursor_thread) {
    CloseHandle(cursor_thread);
  }

  HANDLE cursor_ipc_thread =
      CreateThread(NULL, 0, cursor_ipc_poll_thread, NULL, 0, NULL);
  if (cursor_ipc_thread) {
    CloseHandle(cursor_ipc_thread);
  }
}
