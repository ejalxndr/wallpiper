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

#pragma once

#include <QQuickWindow>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdint>
#include <optional>

QT_FORWARD_DECLARE_CLASS(QSGTexture)

namespace WallpiperKde {

constexpr unsigned int kGlTextureExternalOes = 0x0DE1; // GL_TEXTURE_2D

class EglDmabufImporter {
public:
  struct Import {
    unsigned int texture = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    int syncFd = -1;
  };

  bool ensureBound(QQuickWindow *window);
  bool isBound() const;

  std::optional<Import> importDmabuf(int width, int height, uint32_t stride,
                                     uint64_t modifier, uint32_t drmFourcc,
                                     int fd) const;
  void destroyImport(const Import &import) const;
  void refreshBinding(const Import &import) const;

  std::optional<EGLImageKHR> createImageOnly(int width, int height,
                                             uint32_t stride, uint64_t modifier,
                                             uint32_t drmFourcc, int fd) const;
  void destroyEglImage(EGLImageKHR image) const;
  bool waitForSyncFd(int syncFd) const;

  bool queryRenderNode(uint32_t *major, uint32_t *minor) const;

  QSGTexture *wrapExternalOes(QQuickWindow *window, const Import &import,
                              const QSize &size) const;
  QSGTexture *wrapTexture2D(QQuickWindow *window, unsigned int texture,
                            const QSize &size) const;

private:
  using GlEglImageTargetTexture2DOesFn = void (*)(unsigned int target,
                                                  void *image);
  using EglCreateSyncKHRFn = EGLSyncKHR (*)(EGLDisplay, EGLenum,
                                            const EGLint *);
  using EglDestroySyncKHRFn = EGLBoolean (*)(EGLDisplay, EGLSyncKHR);
  using EglClientWaitSyncKHRFn = EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint,
                                            EGLTimeKHR);

  EGLDisplay m_display = EGL_NO_DISPLAY;
  GlEglImageTargetTexture2DOesFn m_glEGLImageTargetTexture2DOES = nullptr;
  EglCreateSyncKHRFn m_eglCreateSyncKHR = nullptr;
  EglDestroySyncKHRFn m_eglDestroySyncKHR = nullptr;
  EglClientWaitSyncKHRFn m_eglClientWaitSyncKHR = nullptr;
  bool m_loggedUnsupported = false;
};

} // namespace WallpiperKde
