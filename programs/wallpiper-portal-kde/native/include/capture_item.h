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

#include "egl_dmabuf_importer.h"
#include "protocol.h"
#include "vulkan_dmabuf_importer.h"

#include <QQuickItem>
#include <QSGTexture>
#include <qopengl.h>
#include <qqmlintegration.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace WallpiperKde {

struct BlitProgramState {
  GLuint program = 0;
  GLint texLoc = 0;
  GLuint vao = 0;
  GLuint vbo = 0;
  bool ready = false;
  bool failed = false;
};

struct CaptureCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  QString err;
};

class WallpaperCaptureItem : public QQuickItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(CaptureItem)

  Q_PROPERTY(bool debugEnabled READ debugEnabled NOTIFY debugEnabledChanged)
  Q_PROPERTY(int displayFps READ displayFps NOTIFY statsChanged)
  Q_PROPERTY(int captureFps READ captureFps NOTIFY statsChanged)
  Q_PROPERTY(double lastFrameMs READ lastFrameMs NOTIFY statsChanged)
  Q_PROPERTY(double peakFrameMs READ peakFrameMs NOTIFY statsChanged)

public:
  explicit WallpaperCaptureItem(QQuickItem *parent = nullptr);
  ~WallpaperCaptureItem() override;

  void componentComplete() override;

  void stageBuf(quint32 slot, quint32 width, quint32 height, quint32 format,
                quint32 stride, quint64 modifier, int fd, int syncFd);
  void stageFrame(quint32 slot, int syncFd);
  void stageShm(quint32 width, quint32 height, quint32 stride, int fd);
  void clearDisplay();
  void requestDetach();
  void setDebugEnabled(bool enabled);

  std::shared_ptr<CaptureCompletion> beginCapture(const QString &path);

  bool debugEnabled() const { return m_debugEnabled; }
  int displayFps() const { return m_displayFps; }
  int captureFps() const { return m_captureFps; }
  double lastFrameMs() const { return m_lastFrameMs; }
  double peakFrameMs() const { return m_peakFrameMs; }

  std::optional<WallpiperProtocol::MonitorGeometry> currentGeometry() const;
  bool queryRenderNode(uint32_t *major, uint32_t *minor) const;

signals:
  void debugEnabledChanged();
  void statsChanged();

protected:
  QSGNode *updatePaintNode(QSGNode *oldNode,
                           UpdatePaintNodeData *data) override;

private:
  enum class Backend { None, Egl, Vulkan };

  struct SlotTexture {
    std::optional<EglDmabufImporter::Import> eglImport;
    std::optional<VulkanDmabufImporter::Import> vkImport;
    quint32 width = 0;
    quint32 height = 0;
    quint32 format = 0;
    quint32 stride = 0;
    quint64 modifier = 0;
    int memFd = -1;
    unsigned int blitTexture = 0;
    unsigned int blitFbo = 0;
    std::unique_ptr<QSGTexture> sgTexture;
  };

  struct PendingBuf {
    quint32 slot = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 format = 0;
    quint32 stride = 0;
    quint64 modifier = 0;
    int fd = -1;
  };

  struct PendingShm {
    quint32 width = 0;
    quint32 height = 0;
    quint32 stride = 0;
    int fd = -1;
  };

  struct PendingSource {
    bool isShm = false;
    quint32 slot = 0;
    int syncFd = -1;
  };

  struct PendingCapture {
    QString path;
    std::shared_ptr<CaptureCompletion> completion;
  };

  void destroySlot(quint32 slot);
  void destroyAllSlots();
  bool reimportSlot(quint32 slot);
  void replacePendingSource(std::optional<PendingSource> next);
  void requestUpdate();
  void recordCapture();
  void recordDisplay();
  static void pruneWindow(std::deque<qint64> &timestamps, qint64 nowMs,
                          qint64 windowMs);
  static int countWithin(const std::deque<qint64> &timestamps, qint64 nowMs,
                         qint64 windowMs);

  EglDmabufImporter m_importer;
  VulkanDmabufImporter m_vkImporter;
  Backend m_backend = Backend::None;
  BlitProgramState m_blitProgram;

  std::unordered_map<quint32, SlotTexture> m_slotTextures;
  std::unique_ptr<QSGTexture> m_shmTexture;

  std::deque<PendingBuf> m_pendingBufs;
  std::optional<PendingShm> m_pendingShm;
  std::optional<PendingSource> m_pendingSource;
  bool m_pendingDetach = false;

  std::mutex m_pendingCaptureMutex;
  std::optional<PendingCapture> m_pendingCapture;

  std::optional<quint32> m_currentSlot;
  bool m_currentIsShm = false;

  std::optional<quint32> m_loggedFormat;

  bool m_debugEnabled = false;
  int m_displayFps = 0;
  int m_captureFps = 0;
  double m_lastFrameMs = 0.0;
  double m_peakFrameMs = 0.0;
  std::deque<qint64> m_displayTimestampsMs;
  std::deque<qint64> m_captureTimestampsMs;
};

} // namespace WallpiperKde
