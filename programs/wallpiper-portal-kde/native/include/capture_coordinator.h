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

#include "protocol.h"

#include <QObject>

#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

QT_FORWARD_DECLARE_CLASS(QScreen)
QT_FORWARD_DECLARE_CLASS(QTimer)

namespace WallpiperKde {

class CaptureSocket;
class CtlListener;
class NativeWindowWatcher;
class WallpaperCaptureItem;

class CaptureCoordinator : public QObject {
  Q_OBJECT
public:
  static CaptureCoordinator *instance();

  void registerItem(WallpaperCaptureItem *item);
  void unregisterItem(WallpaperCaptureItem *item);
  void reevaluateActiveItem();
  bool captureChannel(uint32_t channel, const QString &path, QString &err);
  std::optional<uint32_t> channelForScreen(const QScreen *screen) const;

  void setWallpaperCursor(int32_t x, int32_t y);
  void clearWallpaperCursor();

private:
  explicit CaptureCoordinator(QObject *parent = nullptr);
  ~CaptureCoordinator() override;

  void ensureSocketsBound();
  void teardownSockets();

  WallpaperCaptureItem *channelItem(uint32_t channel) const;
  WallpaperCaptureItem *claimItemForPosition(uint32_t channel, qint32 x,
                                             qint32 y);
  WallpaperCaptureItem *claimItemForSize(uint32_t channel, quint32 width,
                                         quint32 height);
  WallpaperCaptureItem *claimItemByOutputName(uint32_t channel,
                                              const QString &name);
  WallpaperCaptureItem *claimItem(uint32_t channel, quint32 width,
                                  quint32 height, bool hasGeometry,
                                  qint32 geomX, qint32 geomY);

  std::optional<WallpiperProtocol::MonitorGeometry>
  geometryFromActiveItem() const;
  void handleDetach();
  void handleSetDebug(bool enabled);

  void handleBuf(quint32 slot, quint32 width, quint32 height, quint32 format,
                quint32 stride, quint64 modifier, bool hasGeometry,
                qint32 geomX, qint32 geomY, int fd, int syncFd);
  void retryPendingBufs();

  struct PendingBuf {
    quint32 width = 0;
    quint32 height = 0;
    quint32 format = 0;
    quint32 stride = 0;
    quint64 modifier = 0;
    bool hasGeometry = false;
    qint32 geomX = 0;
    qint32 geomY = 0;
    int fd = -1;
    int syncFd = -1;
  };

  std::vector<WallpaperCaptureItem *> m_items;
  std::unordered_map<uint32_t, WallpaperCaptureItem *> m_channelItems;
  WallpaperCaptureItem *m_primaryItem = nullptr;

  std::unordered_map<quint32, PendingBuf> m_pendingBufs;
  std::unordered_set<uint32_t> m_loggedDroppedChannels;

  CaptureSocket *m_captureSocket = nullptr;
  CtlListener *m_ctlListener = nullptr;

  QTimer *m_cursorTimer = nullptr;
  QTimer *m_pendingRetryTimer = nullptr;
  NativeWindowWatcher *m_nativeWindowWatcher = nullptr;
  std::atomic<int32_t> m_cursorX{0};
  std::atomic<int32_t> m_cursorY{0};
  std::atomic<bool> m_wallpaperCursorActive{false};
};

} // namespace WallpiperKde
