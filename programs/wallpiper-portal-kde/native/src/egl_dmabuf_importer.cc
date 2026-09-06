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

#include "egl_dmabuf_importer.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <qopenglcontext_platform.h>
#include <qsgtexture_platform.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

namespace WallpiperKde {

namespace {
struct FdCloser {
  int fd;
  ~FdCloser() { ::close(fd); }
};

void waitForDmabufWrite(int fd) {
  if (fd < 0) {
    return;
  }
  dma_buf_sync sync{};
  sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
  if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
    return;
  }
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
} // namespace

bool EglDmabufImporter::isBound() const {
  return m_display != EGL_NO_DISPLAY &&
         m_glEGLImageTargetTexture2DOES != nullptr;
}

bool EglDmabufImporter::ensureBound(QQuickWindow *window) {
  if (isBound()) {
    return true;
  }
  if (!window) {
    return false;
  }

  auto *rif = window->rendererInterface();
  if (!rif || rif->graphicsApi() != QSGRendererInterface::OpenGL) {
    if (!m_loggedUnsupported) {
      qWarning() << "[egl] Qt Quick scenegraph is not on the OpenGL RHI "
                    "backend (graphicsApi ="
                 << (rif ? rif->graphicsApi() : QSGRendererInterface::Unknown)
                 << "). dmabuf zero-copy import requires it. Set "
                    "QSG_RHI_BACKEND=opengl in "
                    "plasmashell's environment (e.g. "
                    "~/.config/plasma-workspace/env/) and "
                    "restart plasmashell.";
      m_loggedUnsupported = true;
    }
    return false;
  }

  auto *ctx = reinterpret_cast<QOpenGLContext *>(
      rif->getResource(window, QSGRendererInterface::OpenGLContextResource));
  if (!ctx) {
    qWarning() << "[egl] no QOpenGLContext available from the scenegraph";
    return false;
  }

  auto *eglIface = ctx->nativeInterface<QNativeInterface::QEGLContext>();
  if (!eglIface) {
    qWarning() << "[egl] QOpenGLContext has no QEGLContext native interface "
                  "(not running on EGL?)";
    return false;
  }

  EGLDisplay display = eglIface->display();
  auto proc = ctx->getProcAddress("glEGLImageTargetTexture2DOES");
  if (!proc) {
    qWarning() << "[egl] glEGLImageTargetTexture2DOES not available "
                  "(GL_OES_EGL_image_external unsupported?)";
    return false;
  }

  m_display = display;
  m_glEGLImageTargetTexture2DOES =
      reinterpret_cast<GlEglImageTargetTexture2DOesFn>(proc);
  m_eglCreateSyncKHR = reinterpret_cast<EglCreateSyncKHRFn>(
      eglGetProcAddress("eglCreateSyncKHR"));
  m_eglDestroySyncKHR = reinterpret_cast<EglDestroySyncKHRFn>(
      eglGetProcAddress("eglDestroySyncKHR"));
  m_eglClientWaitSyncKHR = reinterpret_cast<EglClientWaitSyncKHRFn>(
      eglGetProcAddress("eglClientWaitSyncKHR"));
  if (!m_eglCreateSyncKHR || !m_eglDestroySyncKHR || !m_eglClientWaitSyncKHR) {
    qWarning() << "[egl] EGL_KHR_fence_sync / EGL_ANDROID_native_fence_sync "
                  "not available, "
                  "dmabuf frames will not be explicitly synchronized";
  }
  qInfo() << "[egl] bound EGL dmabuf importer, display" << m_display;
  return true;
}

std::optional<EglDmabufImporter::Import>
EglDmabufImporter::importDmabuf(int width, int height, uint32_t stride,
                                uint64_t modifier, uint32_t drmFourcc,
                                int fd) const {
  const FdCloser fdCloser{fd};

  if (!isBound()) {
    return std::nullopt;
  }

  const auto modifierLo = static_cast<EGLAttrib>(modifier & 0xffffffffu);
  const auto modifierHi = static_cast<EGLAttrib>(modifier >> 32);

  const EGLAttrib attribs[] = {
      EGL_WIDTH,
      width,
      EGL_HEIGHT,
      height,
      EGL_LINUX_DRM_FOURCC_EXT,
      static_cast<EGLAttrib>(drmFourcc),
      EGL_DMA_BUF_PLANE0_FD_EXT,
      fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      static_cast<EGLAttrib>(stride),
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      modifierLo,
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      modifierHi,
      EGL_NONE,
  };

  EGLImage image = eglCreateImage(m_display, EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
  if (image == EGL_NO_IMAGE) {
    qWarning() << "[egl] eglCreateImage failed, EGL error" << Qt::hex
               << eglGetError();
    return std::nullopt;
  }

  auto *ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    qWarning() << "[egl] no current QOpenGLContext during dmabuf import";
    eglDestroyImage(m_display, image);
    return std::nullopt;
  }
  auto *gl = ctx->functions();

  GLuint texture = 0;
  gl->glGenTextures(1, &texture);
  gl->glBindTexture(kGlTextureExternalOes, texture);
  gl->glTexParameteri(kGlTextureExternalOes, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->glTexParameteri(kGlTextureExternalOes, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->glTexParameteri(kGlTextureExternalOes, GL_TEXTURE_WRAP_S,
                      GL_CLAMP_TO_EDGE);
  gl->glTexParameteri(kGlTextureExternalOes, GL_TEXTURE_WRAP_T,
                      GL_CLAMP_TO_EDGE);
  m_glEGLImageTargetTexture2DOES(kGlTextureExternalOes, image);
  gl->glBindTexture(kGlTextureExternalOes, 0);

  Import result;
  result.texture = texture;
  result.image = image;
  result.syncFd = ::dup(fd);
  return result;
}

std::optional<EGLImageKHR>
EglDmabufImporter::createImageOnly(int width, int height, uint32_t stride,
                                   uint64_t modifier, uint32_t drmFourcc,
                                   int fd) const {
  const FdCloser fdCloser{fd};

  if (!isBound()) {
    return std::nullopt;
  }

  const auto modifierLo = static_cast<EGLAttrib>(modifier & 0xffffffffu);
  const auto modifierHi = static_cast<EGLAttrib>(modifier >> 32);

  const EGLAttrib attribs[] = {
      EGL_WIDTH,
      width,
      EGL_HEIGHT,
      height,
      EGL_LINUX_DRM_FOURCC_EXT,
      static_cast<EGLAttrib>(drmFourcc),
      EGL_DMA_BUF_PLANE0_FD_EXT,
      fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      static_cast<EGLAttrib>(stride),
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      modifierLo,
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      modifierHi,
      EGL_NONE,
  };

  EGLImage image = eglCreateImage(m_display, EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
  if (image == EGL_NO_IMAGE) {
    qWarning() << "[egl] eglCreateImage (refresh) failed, EGL error" << Qt::hex
               << eglGetError();
    return std::nullopt;
  }
  return image;
}

void EglDmabufImporter::destroyEglImage(EGLImageKHR image) const {
  if (isBound() && image != EGL_NO_IMAGE) {
    eglDestroyImage(m_display, image);
  }
}

void EglDmabufImporter::refreshBinding(const Import &import) const {
  if (!isBound()) {
    return;
  }
  auto *ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    return;
  }
  waitForDmabufWrite(import.syncFd);
  auto *gl = ctx->functions();
  gl->glBindTexture(kGlTextureExternalOes, import.texture);
  m_glEGLImageTargetTexture2DOES(kGlTextureExternalOes, import.image);
  gl->glBindTexture(kGlTextureExternalOes, 0);
}

bool EglDmabufImporter::waitForSyncFd(int syncFd) const {
  if (syncFd < 0) {
    return true;
  }
  if (!isBound() || !m_eglCreateSyncKHR || !m_eglClientWaitSyncKHR ||
      !m_eglDestroySyncKHR) {
    ::close(syncFd);
    return false;
  }

  const EGLint attribs[] = {
      EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
      syncFd,
      EGL_NONE,
  };
  EGLSyncKHR sync =
      m_eglCreateSyncKHR(m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
  if (sync == EGL_NO_SYNC_KHR) {
    qWarning() << "[egl] eglCreateSyncKHR failed, EGL error" << Qt::hex
               << eglGetError();
    ::close(syncFd);
    return false;
  }

  constexpr EGLTimeKHR kWaitTimeoutNs = 200'000'000;
  EGLint result = m_eglClientWaitSyncKHR(
      m_display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, kWaitTimeoutNs);
  m_eglDestroySyncKHR(m_display, sync);

  if (result != EGL_CONDITION_SATISFIED_KHR) {
    qWarning() << "[egl] eglClientWaitSyncKHR did not signal in time, result"
               << Qt::hex << result;
    return false;
  }
  return true;
}

bool EglDmabufImporter::queryRenderNode(uint32_t *major,
                                        uint32_t *minor) const {
  if (!isBound()) {
    return false;
  }

  using EglQueryDisplayAttribEXTFn = EGLBoolean (*)(EGLDisplay, EGLint,
                                                    EGLAttrib *);
  using EglQueryDeviceStringEXTFn = const char *(*)(EGLDeviceEXT, EGLint);

  auto queryDisplayAttrib = reinterpret_cast<EglQueryDisplayAttribEXTFn>(
      eglGetProcAddress("eglQueryDisplayAttribEXT"));
  auto queryDeviceString = reinterpret_cast<EglQueryDeviceStringEXTFn>(
      eglGetProcAddress("eglQueryDeviceStringEXT"));
  if (!queryDisplayAttrib || !queryDeviceString) {
    return false;
  }

  EGLAttrib deviceAttrib = 0;
  if (!queryDisplayAttrib(m_display, EGL_DEVICE_EXT, &deviceAttrib)) {
    return false;
  }
  auto device = reinterpret_cast<EGLDeviceEXT>(deviceAttrib);

  const char *path = queryDeviceString(device, EGL_DRM_RENDER_NODE_FILE_EXT);
  if (!path) {
    path = queryDeviceString(device, EGL_DRM_DEVICE_FILE_EXT);
  }
  if (!path) {
    return false;
  }

  struct stat st{};
  if (::stat(path, &st) != 0) {
    return false;
  }
  *major = ::major(st.st_rdev);
  *minor = ::minor(st.st_rdev);
  return true;
}

void EglDmabufImporter::destroyImport(const Import &import) const {
  if (auto *ctx = QOpenGLContext::currentContext()) {
    GLuint texture = import.texture;
    ctx->functions()->glDeleteTextures(1, &texture);
  }
  if (isBound() && import.image != EGL_NO_IMAGE) {
    eglDestroyImage(m_display, import.image);
  }
  if (import.syncFd >= 0) {
    ::close(import.syncFd);
  }
}

QSGTexture *EglDmabufImporter::wrapExternalOes(QQuickWindow *window,
                                               const Import &import,
                                               const QSize &size) const {
  return QNativeInterface::QSGOpenGLTexture::fromNativeExternalOES(
      import.texture, window, size);
}

QSGTexture *EglDmabufImporter::wrapTexture2D(QQuickWindow *window,
                                             unsigned int texture,
                                             const QSize &size) const {
  return QNativeInterface::QSGOpenGLTexture::fromNative(texture, window, size);
}

} // namespace WallpiperKde
