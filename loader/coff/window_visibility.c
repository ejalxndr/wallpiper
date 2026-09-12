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

#include "window_visibility.h"

#include "pe_iat.h"
#include "progman.h"
#include "util.h"

typedef HWND(WINAPI *CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
typedef BOOL(WINAPI *ShowWindow_t)(HWND, int);
typedef BOOL(WINAPI *SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);

static CreateWindowExW_t real_CreateWindowExW;
static ShowWindow_t real_ShowWindow;
static SetWindowPos_t real_SetWindowPos;

static BOOL is_under_fake_workerw(HWND hwnd) {
  if (!g_fake_empty_workerw) {
    return FALSE;
  }
  for (int depth = 0; hwnd && depth < 32; depth++) {
    if (hwnd == g_fake_empty_workerw) {
      return TRUE;
    }
    hwnd = GetParent(hwnd);
  }
  return FALSE;
}

static HWND WINAPI fake_CreateWindowExW(DWORD exStyle, LPCWSTR className,
                                        LPCWSTR windowName, DWORD style, int x,
                                        int y, int w, int h, HWND parent,
                                        HMENU menu, HINSTANCE hinst,
                                        LPVOID param) {
  if (is_under_fake_workerw(parent)) {
    style &= ~WS_VISIBLE;
  }
  return real_CreateWindowExW ? real_CreateWindowExW(exStyle, className,
                                                     windowName, style, x, y,
                                                     w, h, parent, menu, hinst,
                                                     param)
                             : NULL;
}

static BOOL WINAPI fake_ShowWindow(HWND hwnd, int cmdShow) {
  if (cmdShow != SW_HIDE && is_under_fake_workerw(hwnd)) {
    return TRUE;
  }
  return real_ShowWindow ? real_ShowWindow(hwnd, cmdShow) : FALSE;
}

static BOOL WINAPI fake_SetWindowPos(HWND hwnd, HWND insertAfter, int x, int y,
                                     int cx, int cy, UINT flags) {
  if ((flags & SWP_SHOWWINDOW) && is_under_fake_workerw(hwnd)) {
    flags &= ~SWP_SHOWWINDOW;
  }
  return real_SetWindowPos ? real_SetWindowPos(hwnd, insertAfter, x, y, cx, cy,
                                               flags)
                          : FALSE;
}

void install_window_visibility_hooks(void) {
  FARPROC origCWEW = NULL;
  int nCWEW = patch_iat_all_modules(
      "USER32.dll", "CreateWindowExW", (FARPROC)(void *)fake_CreateWindowExW,
      &origCWEW);
  real_CreateWindowExW = (CreateWindowExW_t)(void *)origCWEW;
  {
    char b[96];
    wsprintfA(b,
              "install_window_visibility_hooks: patch_iat_all_modules "
              "CreateWindowExW patched=%d modules",
              nCWEW);
    debug_log(b);
  }

  FARPROC origSW = NULL;
  int nSW = patch_iat_all_modules(
      "USER32.dll", "ShowWindow", (FARPROC)(void *)fake_ShowWindow, &origSW);
  real_ShowWindow = (ShowWindow_t)(void *)origSW;
  {
    char b[96];
    wsprintfA(b,
              "install_window_visibility_hooks: patch_iat_all_modules "
              "ShowWindow patched=%d modules",
              nSW);
    debug_log(b);
  }

  FARPROC origSWP = NULL;
  int nSWP = patch_iat_all_modules(
      "USER32.dll", "SetWindowPos", (FARPROC)(void *)fake_SetWindowPos,
      &origSWP);
  real_SetWindowPos = (SetWindowPos_t)(void *)origSWP;
  {
    char b[96];
    wsprintfA(b,
              "install_window_visibility_hooks: patch_iat_all_modules "
              "SetWindowPos patched=%d modules",
              nSWP);
    debug_log(b);
  }
}
