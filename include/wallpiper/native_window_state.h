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

/*
 * Wire format shared by the KDE portal (writer) and the dwmapi shim (reader)
 * inside wallpaper64.exe.
 *
 * The old format was two uint32 masks indexed by capture channel.  Channels
 * follow Wallpaper Engine's swapchain creation order, which is not stable
 * across launches, while WE's playback masks are indexed by its own monitor
 * order.  Publish screen rects instead so the shim can derive the monitor
 * indices with EnumDisplayMonitors in the same process as WE.
 *
 * Coordinates are native (X11/Wayland) logical pixels; the shim translates
 * them into Wine coordinates with GetSystemMetrics(SM_XVIRTUALSCREEN/Y...).
 * All fields are 32-bit on both the Qt and the freestanding Windows builds.
 */

#ifndef WALLPIPER_NATIVE_WINDOW_STATE_H
#define WALLPIPER_NATIVE_WINDOW_STATE_H

#define WP_NATIVE_WINDOW_STATE_MAGIC 0x574e5731u /* "WNW1" */
#define WP_NATIVE_WINDOW_STATE_MAX_SCREENS 8u
#define WP_NATIVE_WINDOW_FLAG_FULLSCREEN 0x1u
#define WP_NATIVE_WINDOW_FLAG_MAXIMIZED 0x2u

typedef struct WpNativeWindowScreen {
  int x;
  int y;
  int width;
  int height;
  unsigned int flags;
} WpNativeWindowScreen;

typedef struct WpNativeWindowState {
  unsigned int magic;
  unsigned int count;
  WpNativeWindowScreen screens[WP_NATIVE_WINDOW_STATE_MAX_SCREENS];
} WpNativeWindowState;

#endif /* WALLPIPER_NATIVE_WINDOW_STATE_H */
