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

#include "capture_item.h"

#include "capture_coordinator.h"

#include <wallpiper/vk_format.h>

#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <thread>

#include <sys/mman.h>
#include <unistd.h>

namespace WallpiperKde {

namespace {
constexpr qint64 kStatsWindowMs = 3000;

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

bool ensureBlitProgram(QOpenGLExtraFunctions *gl, BlitProgramState &state,
                       GLuint &program, GLint &texLoc, GLuint &vao) {
  if (state.failed) {
    return false;
  }
  if (!state.ready) {
    const char *vsSrc = "attribute vec2 pos;\n"
                        "varying vec2 uv;\n"
                        "void main() {\n"
                        "  uv = pos * 0.5 + 0.5;\n"
                        "  gl_Position = vec4(pos, 0.0, 1.0);\n"
                        "}\n";
    const char *fsSrc = "#ifdef GL_ES\n"
                        "precision mediump float;\n"
                        "#endif\n"
                        "uniform sampler2D tex;\n"
                        "varying vec2 uv;\n"
                        "void main() {\n"
                        "  gl_FragColor = texture2D(tex, uv);\n"
                        "}\n";
    GLuint vs = gl->glCreateShader(GL_VERTEX_SHADER);
    gl->glShaderSource(vs, 1, &vsSrc, nullptr);
    gl->glCompileShader(vs);
    GLuint fs = gl->glCreateShader(GL_FRAGMENT_SHADER);
    gl->glShaderSource(fs, 1, &fsSrc, nullptr);
    gl->glCompileShader(fs);

    GLint vsOk = 0;
    GLint fsOk = 0;
    gl->glGetShaderiv(vs, GL_COMPILE_STATUS, &vsOk);
    gl->glGetShaderiv(fs, GL_COMPILE_STATUS, &fsOk);
    if (!vsOk || !fsOk) {
      char log[512];
      gl->glGetShaderInfoLog(fsOk ? vs : fs, sizeof(log), nullptr, log);
      qWarning() << "[capture] blit shader compile failed:" << log;
      state.failed = true;
      return false;
    }

    state.program = gl->glCreateProgram();
    gl->glAttachShader(state.program, vs);
    gl->glAttachShader(state.program, fs);
    gl->glBindAttribLocation(state.program, 0, "pos");
    gl->glLinkProgram(state.program);
    GLint linkOk = 0;
    gl->glGetProgramiv(state.program, GL_LINK_STATUS, &linkOk);
    if (!linkOk) {
      char log[512];
      gl->glGetProgramInfoLog(state.program, sizeof(log), nullptr, log);
      qWarning() << "[capture] blit shader link failed:" << log;
      state.failed = true;
      return false;
    }
    state.texLoc = gl->glGetUniformLocation(state.program, "tex");
    gl->glDeleteShader(vs);
    gl->glDeleteShader(fs);

    gl->glGenVertexArrays(1, &state.vao);
    gl->glBindVertexArray(state.vao);
    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    gl->glGenBuffers(1, &state.vbo);
    gl->glBindBuffer(GL_ARRAY_BUFFER, state.vbo);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindVertexArray(0);

    state.ready = true;
  }

  program = state.program;
  texLoc = state.texLoc;
  vao = state.vao;
  return true;
}

bool blitExternalOesToTexture2D(QOpenGLExtraFunctions *gl,
                                BlitProgramState &blitState,
                                unsigned int oesTexture, unsigned int dstFbo,
                                int width, int height) {
  GLuint program = 0;
  GLint texLoc = 0;
  GLuint vao = 0;
  if (!ensureBlitProgram(gl, blitState, program, texLoc, vao)) {
    return false;
  }

  GLint prevFbo = 0;
  GLint prevViewport[4] = {0, 0, 0, 0};
  GLboolean blendWasEnabled = gl->glIsEnabled(GL_BLEND);
  GLboolean scissorWasEnabled = gl->glIsEnabled(GL_SCISSOR_TEST);
  gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  gl->glGetIntegerv(GL_VIEWPORT, prevViewport);

  gl->glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
  gl->glViewport(0, 0, width, height);
  gl->glDisable(GL_BLEND);
  gl->glDisable(GL_SCISSOR_TEST);
  gl->glUseProgram(program);
  gl->glActiveTexture(GL_TEXTURE0);
  gl->glBindTexture(kGlTextureExternalOes, oesTexture);
  gl->glUniform1i(texLoc, 0);
  gl->glBindVertexArray(vao);
  gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl->glBindVertexArray(0);
  gl->glBindTexture(kGlTextureExternalOes, 0);
  gl->glUseProgram(0);

  if (blendWasEnabled) {
    gl->glEnable(GL_BLEND);
  }
  if (scissorWasEnabled) {
    gl->glEnable(GL_SCISSOR_TEST);
  }
  gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
  gl->glViewport(prevViewport[0], prevViewport[1], prevViewport[2],
                 prevViewport[3]);
  return true;
}
} // namespace

WallpaperCaptureItem::WallpaperCaptureItem(QQuickItem *parent)
    : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
}

WallpaperCaptureItem::~WallpaperCaptureItem() {
  CaptureCoordinator::instance()->unregisterItem(this);
  replacePendingSource(std::nullopt);
  destroyAllSlots();
  if (auto *ctx = QOpenGLContext::currentContext()) {
    auto *gl = ctx->extraFunctions();
    if (m_blitProgram.vao) {
      gl->glDeleteVertexArrays(1, &m_blitProgram.vao);
    }
    if (m_blitProgram.vbo) {
      gl->glDeleteBuffers(1, &m_blitProgram.vbo);
    }
    if (m_blitProgram.program) {
      gl->glDeleteProgram(m_blitProgram.program);
    }
  }
}

void WallpaperCaptureItem::componentComplete() {
  QQuickItem::componentComplete();

  connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *win) {
    if (win) {
      connect(win, &QQuickWindow::screenChanged, this,
              [] { CaptureCoordinator::instance()->reevaluateActiveItem(); });
      if (!win->isSceneGraphInitialized()) {
        auto config = win->graphicsConfiguration();
        config.setDeviceExtensions({
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf",
            "VK_EXT_queue_family_foreign",
            "VK_EXT_image_drm_format_modifier",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
        });
        win->setGraphicsConfiguration(config);
      }
    }
    CaptureCoordinator::instance()->reevaluateActiveItem();
  });
  connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [](QScreen *) {
    CaptureCoordinator::instance()->reevaluateActiveItem();
  });

  CaptureCoordinator::instance()->registerItem(this);
}

void WallpaperCaptureItem::replacePendingSource(
    std::optional<PendingSource> next) {
  if (m_pendingSource && m_pendingSource->syncFd >= 0) {
    ::close(m_pendingSource->syncFd);
  }
  m_pendingSource = next;
}

void WallpaperCaptureItem::requestUpdate() {
  update();
  if (auto *win = window()) {
    win->update();
  }
}

void WallpaperCaptureItem::stageBuf(quint32 slot, quint32 width, quint32 height,
                                    quint32 format, quint32 stride,
                                    quint64 modifier, int fd, int syncFd) {
  if (!m_loggedFormat || *m_loggedFormat != format) {
    m_loggedFormat = format;
    int matched = 0;
    uint32_t fourcc = wp_drm_fourcc_from_vk_format(format, &matched);
    qInfo() << "[capture] wallpaper buffer VkFormat" << format
            << "mapped to DRM fourcc" << Qt::hex << fourcc
            << (matched ? "(known mapping)" : "(unrecognized, defaulting)");
  }
  m_pendingBufs.push_back(
      PendingBuf{slot, width, height, format, stride, modifier, fd});
  replacePendingSource(PendingSource{false, slot, syncFd});
  recordCapture();
  requestUpdate();
}

void WallpaperCaptureItem::stageFrame(quint32 slot, int syncFd) {
  bool knownAlready = m_slotTextures.find(slot) != m_slotTextures.end();
  bool willBeKnown = false;
  for (const auto &pending : m_pendingBufs) {
    if (pending.slot == slot) {
      willBeKnown = true;
      break;
    }
  }
  if (knownAlready || willBeKnown) {
    replacePendingSource(PendingSource{false, slot, syncFd});
  } else if (syncFd >= 0) {
    ::close(syncFd);
  }
  recordCapture();
  requestUpdate();
}

void WallpaperCaptureItem::stageShm(quint32 width, quint32 height,
                                    quint32 stride, int fd) {
  m_pendingShm = PendingShm{width, height, stride, fd};
  replacePendingSource(PendingSource{true, 0, -1});
  recordCapture();
  requestUpdate();
}

void WallpaperCaptureItem::clearDisplay() {
  replacePendingSource(std::nullopt);
  m_currentSlot.reset();
  m_currentIsShm = false;
  update();
}

void WallpaperCaptureItem::requestDetach() {
  m_pendingDetach = true;
  requestUpdate();
}

std::shared_ptr<CaptureCompletion>
WallpaperCaptureItem::beginCapture(const QString &path) {
  auto completion = std::make_shared<CaptureCompletion>();
  {
    std::lock_guard<std::mutex> lock(m_pendingCaptureMutex);
    m_pendingCapture = PendingCapture{path, completion};
  }
  requestUpdate();
  return completion;
}

void WallpaperCaptureItem::setDebugEnabled(bool enabled) {
  if (m_debugEnabled == enabled) {
    return;
  }
  m_debugEnabled = enabled;
  emit debugEnabledChanged();
}

std::optional<WallpiperProtocol::MonitorGeometry>
WallpaperCaptureItem::currentGeometry() const {
  auto *win = window();
  if (!win || !win->screen()) {
    return std::nullopt;
  }
  auto *screen = win->screen();
  QRect geometry = screen->geometry();
  qreal scale = screen->devicePixelRatio();
  if (scale <= 0.0) {
    scale = 1.0;
  }

  WallpiperProtocol::MonitorGeometry result;
  result.x = geometry.x();
  result.y = geometry.y();
  result.width = static_cast<uint32_t>(std::lround(geometry.width() * scale));
  result.height = static_cast<uint32_t>(std::lround(geometry.height() * scale));
  result.logicalWidth = static_cast<uint32_t>(geometry.width());
  result.logicalHeight = static_cast<uint32_t>(geometry.height());
  result.scale = scale;
  return result;
}

bool WallpaperCaptureItem::queryRenderNode(uint32_t *major,
                                           uint32_t *minor) const {
  if (m_backend == Backend::Vulkan) {
    return m_vkImporter.queryRenderNode(major, minor);
  }
  if (m_backend == Backend::Egl) {
    return m_importer.queryRenderNode(major, minor);
  }
  return false;
}

void WallpaperCaptureItem::destroySlot(quint32 slot) {
  auto it = m_slotTextures.find(slot);
  if (it == m_slotTextures.end()) {
    return;
  }
  if (it->second.eglImport) {
    m_importer.destroyImport(*it->second.eglImport);
  }
  if (it->second.vkImport) {
    m_vkImporter.destroyImport(*it->second.vkImport);
  }
  if (auto *ctx = QOpenGLContext::currentContext()) {
    auto *gl = ctx->functions();
    if (it->second.blitTexture) {
      gl->glDeleteTextures(1, &it->second.blitTexture);
    }
    if (it->second.blitFbo) {
      gl->glDeleteFramebuffers(1, &it->second.blitFbo);
    }
  }
  if (it->second.memFd >= 0) {
    ::close(it->second.memFd);
  }
  m_slotTextures.erase(it);
}

bool WallpaperCaptureItem::reimportSlot(quint32 slot) {
  auto it = m_slotTextures.find(slot);
  if (it == m_slotTextures.end() || !it->second.eglImport ||
      it->second.memFd < 0) {
    return false;
  }
  int dupFd = ::dup(it->second.memFd);
  if (dupFd < 0) {
    return false;
  }
  uint32_t fourcc = wp_drm_fourcc_from_vk_format(it->second.format, nullptr);
  auto newImage = m_importer.createImageOnly(
      static_cast<int>(it->second.width), static_cast<int>(it->second.height),
      it->second.stride, it->second.modifier, fourcc, dupFd);
  if (!newImage) {
    qWarning() << "[capture] dmabuf image refresh failed for slot" << slot;
    return false;
  }

  m_importer.destroyEglImage(it->second.eglImport->image);
  it->second.eglImport->image = *newImage;
  return true;
}

void WallpaperCaptureItem::destroyAllSlots() {
  auto *ctx = QOpenGLContext::currentContext();
  for (auto &[slot, tex] : m_slotTextures) {
    if (tex.eglImport) {
      m_importer.destroyImport(*tex.eglImport);
    }
    if (tex.vkImport) {
      m_vkImporter.destroyImport(*tex.vkImport);
    }
    if (ctx) {
      auto *gl = ctx->functions();
      if (tex.blitTexture) {
        gl->glDeleteTextures(1, &tex.blitTexture);
      }
      if (tex.blitFbo) {
        gl->glDeleteFramebuffers(1, &tex.blitFbo);
      }
    }
    if (tex.memFd >= 0) {
      ::close(tex.memFd);
    }
  }
  m_slotTextures.clear();
  m_shmTexture.reset();
}

void WallpaperCaptureItem::recordCapture() {
  qint64 now = nowMs();
  m_captureTimestampsMs.push_back(now);
  pruneWindow(m_captureTimestampsMs, now, kStatsWindowMs);
}

void WallpaperCaptureItem::recordDisplay() {
  qint64 now = nowMs();
  if (!m_displayTimestampsMs.empty()) {
    m_lastFrameMs = static_cast<double>(now - m_displayTimestampsMs.back());
    m_peakFrameMs = std::max(m_peakFrameMs, m_lastFrameMs);
  }
  m_displayTimestampsMs.push_back(now);
  pruneWindow(m_displayTimestampsMs, now, kStatsWindowMs);

  m_displayFps = countWithin(m_displayTimestampsMs, now, 1000);
  m_captureFps = countWithin(m_captureTimestampsMs, now, 1000);
  emit statsChanged();
}

void WallpaperCaptureItem::pruneWindow(std::deque<qint64> &timestamps,
                                       qint64 nowMsValue, qint64 windowMs) {
  while (!timestamps.empty() && nowMsValue - timestamps.front() > windowMs) {
    timestamps.pop_front();
  }
}

int WallpaperCaptureItem::countWithin(const std::deque<qint64> &timestamps,
                                      qint64 nowMsValue, qint64 windowMs) {
  int count = 0;
  for (auto it = timestamps.rbegin(); it != timestamps.rend(); ++it) {
    if (nowMsValue - *it > windowMs) {
      break;
    }
    ++count;
  }
  return count;
}

QSGNode *WallpaperCaptureItem::updatePaintNode(QSGNode *oldNode,
                                               UpdatePaintNodeData *) {
  auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);

  if (m_pendingDetach) {
    destroyAllSlots();
    m_currentSlot.reset();
    m_currentIsShm = false;
    replacePendingSource(std::nullopt);
    m_pendingBufs.clear();
    m_pendingShm.reset();
    m_pendingDetach = false;
    delete node;
    return nullptr;
  }

  auto *win = window();
  if (win && m_backend == Backend::None) {
    auto *rif = win->rendererInterface();
    auto api = rif ? rif->graphicsApi() : QSGRendererInterface::Unknown;
    if (api == QSGRendererInterface::OpenGL) {
      m_backend = Backend::Egl;
    } else if (api == QSGRendererInterface::Vulkan) {
      m_backend = Backend::Vulkan;
    }
  }
  if (win) {
    if (m_backend == Backend::Egl) {
      m_importer.ensureBound(win);
    } else if (m_backend == Backend::Vulkan) {
      m_vkImporter.ensureBound(win);
    }
  }

  std::unordered_map<quint32, bool> freshlyImported;
  while (!m_pendingBufs.empty()) {
    PendingBuf pending = m_pendingBufs.front();
    m_pendingBufs.pop_front();

    destroySlot(pending.slot);

    SlotTexture tex;
    tex.width = pending.width;
    tex.height = pending.height;
    tex.format = pending.format;
    tex.stride = pending.stride;
    tex.modifier = pending.modifier;

    bool imported = false;
    if (m_backend == Backend::Vulkan) {
      auto vkImported = m_vkImporter.importDmabuf(
          static_cast<int>(pending.width), static_cast<int>(pending.height),
          pending.stride, pending.modifier,
          static_cast<VkFormat>(pending.format), pending.fd);
      if (vkImported) {
        tex.vkImport = *vkImported;
        tex.sgTexture.reset(m_vkImporter.wrapTexture2D(
            win, *tex.vkImport,
            QSize(static_cast<int>(pending.width),
                 static_cast<int>(pending.height))));
        imported = true;
      } else {
        qWarning() << "[capture] Vulkan dmabuf import failed for slot"
                   << pending.slot << "(VkFormat" << pending.format << ")";
      }
    } else {
      uint32_t fourcc = wp_drm_fourcc_from_vk_format(pending.format, nullptr);
      int retainedFd = ::dup(pending.fd);
      auto eglImported = m_importer.importDmabuf(
          static_cast<int>(pending.width), static_cast<int>(pending.height),
          pending.stride, pending.modifier, fourcc, pending.fd);
      if (!eglImported) {
        qWarning() << "[capture] dmabuf import failed for slot"
                   << pending.slot << "(VkFormat" << pending.format
                   << "-> fourcc" << Qt::hex << fourcc << Qt::dec << ")";
        if (retainedFd >= 0) {
          ::close(retainedFd);
        }
      } else {
        tex.eglImport = *eglImported;
        tex.memFd = retainedFd;

        if (auto *ctx = QOpenGLContext::currentContext()) {
          auto *gl = ctx->extraFunctions();
          gl->glGenTextures(1, &tex.blitTexture);
          gl->glBindTexture(GL_TEXTURE_2D, tex.blitTexture);
          gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                           static_cast<int>(pending.width),
                           static_cast<int>(pending.height), 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, nullptr);
          gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                              GL_LINEAR);
          gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                              GL_LINEAR);
          gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                              GL_CLAMP_TO_EDGE);
          gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                              GL_CLAMP_TO_EDGE);
          gl->glBindTexture(GL_TEXTURE_2D, 0);

          gl->glGenFramebuffers(1, &tex.blitFbo);
          gl->glBindFramebuffer(GL_FRAMEBUFFER, tex.blitFbo);
          gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, tex.blitTexture, 0);
          gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

          blitExternalOesToTexture2D(
              gl, m_blitProgram, tex.eglImport->texture, tex.blitFbo,
              static_cast<int>(pending.width),
              static_cast<int>(pending.height));

          tex.sgTexture.reset(m_importer.wrapTexture2D(
              win, tex.blitTexture,
              QSize(static_cast<int>(pending.width),
                   static_cast<int>(pending.height))));
        }
        imported = true;
      }
    }

    if (!imported) {
      continue;
    }

    m_slotTextures.emplace(pending.slot, std::move(tex));
    freshlyImported[pending.slot] = true;
  }

  if (m_pendingShm) {
    PendingShm shm = *m_pendingShm;
    m_pendingShm.reset();

    void *mapped = ::mmap(nullptr, static_cast<size_t>(shm.stride) * shm.height,
                          PROT_READ, MAP_SHARED, shm.fd, 0);
    if (mapped == MAP_FAILED) {
      qWarning() << "[capture] mmap of SHM frame failed";
    } else {
      QImage image(static_cast<const uchar *>(mapped),
                   static_cast<int>(shm.width), static_cast<int>(shm.height),
                   static_cast<int>(shm.stride), QImage::Format_RGB32);
      if (win) {
        m_shmTexture.reset(win->createTextureFromImage(image.copy()));
      }
      ::munmap(mapped, static_cast<size_t>(shm.stride) * shm.height);
    }
    ::close(shm.fd);
  }

  int newFrameSyncFd = -1;
  bool haveNewFrame = false;
  if (m_pendingSource) {
    m_currentIsShm = m_pendingSource->isShm;
    m_currentSlot = m_pendingSource->isShm
                        ? std::nullopt
                        : std::make_optional(m_pendingSource->slot);
    newFrameSyncFd = m_pendingSource->syncFd;
    haveNewFrame = true;
    m_pendingSource.reset();
  }

  QSGTexture *texture = nullptr;
  GLuint captureFbo = 0;
  int captureWidth = 0;
  int captureHeight = 0;
  bool captureAvailable = false;
  if (m_currentIsShm) {
    texture = m_shmTexture.get();
  } else if (m_currentSlot) {
    auto it = m_slotTextures.find(*m_currentSlot);
    if (it != m_slotTextures.end()) {
      if (haveNewFrame) {
        if (it->second.vkImport) {
          m_vkImporter.blitFrame(*it->second.vkImport, newFrameSyncFd);
        } else if (it->second.eglImport) {
          m_importer.waitForSyncFd(newFrameSyncFd);
          if (!freshlyImported.count(*m_currentSlot)) {
            reimportSlot(*m_currentSlot);
            it = m_slotTextures.find(*m_currentSlot);
          }
          m_importer.refreshBinding(*it->second.eglImport);
          if (auto *ctx = QOpenGLContext::currentContext()) {
            auto *gl = ctx->extraFunctions();
            blitExternalOesToTexture2D(
                gl, m_blitProgram, it->second.eglImport->texture,
                it->second.blitFbo, static_cast<int>(it->second.width),
                static_cast<int>(it->second.height));
          }
        } else if (newFrameSyncFd >= 0) {
          ::close(newFrameSyncFd);
        }
      }
      texture = it->second.sgTexture.get();
      captureFbo = it->second.blitFbo;
      captureWidth = static_cast<int>(it->second.width);
      captureHeight = static_cast<int>(it->second.height);
      captureAvailable = captureFbo != 0;
    } else if (haveNewFrame && newFrameSyncFd >= 0) {
      ::close(newFrameSyncFd);
    }
  } else if (haveNewFrame && newFrameSyncFd >= 0) {
    ::close(newFrameSyncFd);
  }

  std::optional<PendingCapture> pendingCapture;
  {
    std::lock_guard<std::mutex> lock(m_pendingCaptureMutex);
    pendingCapture = m_pendingCapture;
    m_pendingCapture.reset();
  }
  if (pendingCapture) {
    const QString &capturePath = pendingCapture->path;
    const auto &completion = pendingCapture->completion;
    bool ok = false;
    QString captureErr;
    QImage image;
    if (!captureAvailable && m_backend == Backend::Vulkan) {
      captureErr =
          QStringLiteral("capture is not supported on the Vulkan backend yet");
    } else if (!captureAvailable) {
      captureErr = QStringLiteral("no active wallpaper frame to capture");
    } else if (auto *ctx = QOpenGLContext::currentContext()) {
      auto *gl = ctx->extraFunctions();
      image = QImage(captureWidth, captureHeight, QImage::Format_RGBA8888);
      GLint prevFbo = 0;
      gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
      gl->glBindFramebuffer(GL_FRAMEBUFFER, captureFbo);
      gl->glReadPixels(0, 0, captureWidth, captureHeight, GL_RGBA,
                       GL_UNSIGNED_BYTE, image.bits());
      gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
      ok = true;
    } else {
      captureErr = QStringLiteral("no OpenGL context on render thread");
    }

    if (ok) {
      std::thread([image, capturePath, completion]() mutable {
        bool saved = image.save(capturePath, "PNG");
        QString err;
        if (!saved) {
          err = QStringLiteral("failed to write PNG to %1").arg(capturePath);
        }
        {
          std::lock_guard<std::mutex> lock(completion->mutex);
          completion->ok = saved;
          completion->err = err;
          completion->done = true;
        }
        completion->cv.notify_all();
      }).detach();
    } else {
      std::lock_guard<std::mutex> lock(completion->mutex);
      completion->ok = false;
      completion->err = captureErr;
      completion->done = true;
      completion->cv.notify_all();
    }
  }

  if (!texture) {
    delete node;
    return nullptr;
  }

  if (!node) {
    node = new QSGSimpleTextureNode();
    node->setOwnsTexture(false);
  }
  node->setTexture(texture);
  node->setRect(boundingRect());
  node->markDirty(QSGNode::DirtyMaterial | QSGNode::DirtyForceUpdate);
  recordDisplay();
  return node;
}

} // namespace WallpiperKde
