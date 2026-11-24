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

#include "x11_output_lookup.h"
#include "wallpiper-wl/portal.h"

#include <string.h>

#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

static Display *get_x11_display(void) {
  static __thread Display *dpy = NULL;
  static __thread bool tried = false;
  if (!tried) {
    tried = true;
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
      printf("[x11] XOpenDisplay failed, X11-space output lookups "
             "unavailable\n");
    }
  }
  return dpy;
}

bool wp_wl_x11_output_for_position(int32_t x, int32_t y, char *name_out,
                                   size_t name_out_len) {
  name_out[0] = '\0';

  Display *dpy = get_x11_display();
  if (!dpy) {
    return false;
  }

  Window root = DefaultRootWindow(dpy);
  XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) {
    return false;
  }

  bool found = false;
  for (int i = 0; i < res->noutput && !found; i++) {
    XRROutputInfo *output_info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (!output_info) {
      continue;
    }
    if (output_info->connection == RR_Connected && output_info->crtc != None) {
      XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dpy, res, output_info->crtc);
      if (crtc_info) {
        if (crtc_info->x == x && crtc_info->y == y) {
          snprintf(name_out, name_out_len, "%s", output_info->name);
          found = true;
        }
        XRRFreeCrtcInfo(crtc_info);
      }
    }
    XRRFreeOutputInfo(output_info);
  }

  XRRFreeScreenResources(res);
  return found;
}

bool wp_wl_x11_output_for_size(uint32_t width, uint32_t height, char *name_out,
                               size_t name_out_len) {
  name_out[0] = '\0';

  Display *dpy = get_x11_display();
  if (!dpy) {
    return false;
  }

  Window root = DefaultRootWindow(dpy);
  XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) {
    return false;
  }

  bool found = false;
  for (int i = 0; i < res->noutput && !found; i++) {
    XRROutputInfo *output_info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (!output_info) {
      continue;
    }
    if (output_info->connection == RR_Connected && output_info->crtc != None) {
      XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dpy, res, output_info->crtc);
      if (crtc_info) {
        bool straight = (uint32_t)crtc_info->width == width &&
                        (uint32_t)crtc_info->height == height;
        bool rotated = (uint32_t)crtc_info->width == height &&
                       (uint32_t)crtc_info->height == width &&
                       (crtc_info->rotation & (RR_Rotate_90 | RR_Rotate_270));
        if (straight || rotated) {
          snprintf(name_out, name_out_len, "%s", output_info->name);
          found = true;
        }
        XRRFreeCrtcInfo(crtc_info);
      }
    }
    XRRFreeOutputInfo(output_info);
  }

  XRRFreeScreenResources(res);
  return found;
}

bool wp_wl_x11_output_rect(const char *name, int32_t *x, int32_t *y,
                           uint32_t *width, uint32_t *height) {
  Display *dpy = get_x11_display();
  if (!dpy) {
    return false;
  }

  Window root = DefaultRootWindow(dpy);
  XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) {
    return false;
  }

  bool found = false;
  for (int i = 0; i < res->noutput && !found; i++) {
    XRROutputInfo *output_info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (!output_info) {
      continue;
    }
    if (output_info->connection == RR_Connected && output_info->crtc != None &&
        strcmp(output_info->name, name) == 0) {
      XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dpy, res, output_info->crtc);
      if (crtc_info) {
        *x = crtc_info->x;
        *y = crtc_info->y;
        *width = crtc_info->width;
        *height = crtc_info->height;
        found = true;
        XRRFreeCrtcInfo(crtc_info);
      }
    }
    XRRFreeOutputInfo(output_info);
  }

  XRRFreeScreenResources(res);
  return found;
}
