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

#include "capture_coordinator.h"

#include "capture_item.h"
#include "capture_socket.h"
#include "ctl_listener.h"
#include "x11_output_lookup.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <unistd.h>

namespace WallpiperKde {

namespace {
constexpr int kCursorSampleIntervalMs = 8;
constexpr int kPendingBufRetryIntervalMs = 250;

/* must match loader/vk-layer-hook/config.h */
constexpr uint32_t kCaptureSlotCount = 3;

/*
 * a buffered tolerance, as fractional scaling mathmatically floats,
 * we could differ by a pixel on either end.
 */
constexpr int32_t kGeometryTolerancePx = 2;

bool nearlyEqual(int64_t a, int64_t b) {
  return std::abs(a - b) <= kGeometryTolerancePx;
}

/*
 * X11 only has one global scale factor, which translates over to KWins Xwayland
 * policy. Needed to match synthetic monitor dimensions.
 */
double xwaylandGlobalScale() {
  QScreen *primary = QGuiApplication::primaryScreen();
  double scale = primary ? primary->devicePixelRatio() : 1.0;
  return scale > 0.0 ? scale : 1.0;
}

WallpiperProtocol::MonitorGeometry
predictedXWaylandRect(const WallpiperProtocol::MonitorGeometry &real,
                      double globalScale) {
  WallpiperProtocol::MonitorGeometry predicted;
  predicted.x = static_cast<int32_t>(std::lround(real.x * globalScale));
  predicted.y = static_cast<int32_t>(std::lround(real.y * globalScale));
  predicted.width =
      static_cast<uint32_t>(std::lround(real.logicalWidth * globalScale));
  predicted.height =
      static_cast<uint32_t>(std::lround(real.logicalHeight * globalScale));
  return predicted;
}
} // namespace

CaptureCoordinator *CaptureCoordinator::instance() {
  static CaptureCoordinator *coordinator = new CaptureCoordinator();
  return coordinator;
}

CaptureCoordinator::CaptureCoordinator(QObject *parent)
    : QObject(parent), m_captureSocket(new CaptureSocket(this)),
      m_ctlListener(new CtlListener(QStringLiteral("kde"), this)),
      m_cursorTimer(new QTimer(this)),
      m_pendingRetryTimer(new QTimer(this)) {
  connect(m_captureSocket, &CaptureSocket::bufReceived, this,
          [this](quint32 slot, quint32 width, quint32 height, quint32 format,
                 quint32 stride, quint64 modifier, bool hasGeometry,
                 qint32 geomX, qint32 geomY, int fd, int syncFd) {
            handleBuf(slot, width, height, format, stride, modifier,
                     hasGeometry, geomX, geomY, fd, syncFd);
          });
  connect(m_captureSocket, &CaptureSocket::frameReceived, this,
          [this](quint32 slot, int syncFd) {
            uint32_t channel = slot / kCaptureSlotCount;
            if (WallpaperCaptureItem *item = channelItem(channel)) {
              item->stageFrame(slot, syncFd);
            } else {
              if (syncFd >= 0) {
                ::close(syncFd);
              }
              if (m_loggedDroppedChannels.insert(channel).second) {
                qWarning()
                    << "[coordinator] dropping frames for channel" << channel
                    << "- output not bound yet (further drops on this "
                       "channel won't be logged)";
              }
            }
          });
  connect(m_captureSocket, &CaptureSocket::shmReceived, this,
          [this](quint32 width, quint32 height, quint32 stride, int fd) {
            if (m_primaryItem) {
              m_primaryItem->stageShm(width, height, stride, fd);
            } else {
              ::close(fd);
            }
          });

  m_ctlListener->setCursorPosProvider(
      [this]() -> std::optional<std::pair<int32_t, int32_t>> {
        return std::make_pair(m_cursorX.load(), m_cursorY.load());
      });
  m_ctlListener->setGeometryProvider(
      [this]() -> std::optional<WallpiperProtocol::MonitorGeometry> {
        std::optional<WallpiperProtocol::MonitorGeometry> result;
        QMetaObject::invokeMethod(
            this, [this, &result]() { result = geometryFromActiveItem(); },
            Qt::BlockingQueuedConnection);
        return result;
      });
  m_ctlListener->setRenderNodeProvider(
      [this]() -> std::optional<std::pair<uint32_t, uint32_t>> {
        std::optional<std::pair<uint32_t, uint32_t>> result;
        QMetaObject::invokeMethod(
            this,
            [this, &result]() {
              if (!m_primaryItem) {
                return;
              }
              uint32_t major = 0, minor = 0;
              if (m_primaryItem->queryRenderNode(&major, &minor)) {
                result = std::make_pair(major, minor);
              }
            },
            Qt::BlockingQueuedConnection);
        return result;
      });
  m_ctlListener->setDetachHandler([this]() {
    QMetaObject::invokeMethod(
        this, [this]() { handleDetach(); }, Qt::BlockingQueuedConnection);
  });
  m_ctlListener->setDebugHandler([this](bool enabled) {
    QMetaObject::invokeMethod(
        this, [this, enabled]() { handleSetDebug(enabled); },
        Qt::BlockingQueuedConnection);
  });
  m_ctlListener->setCaptureHandler(
      [this](uint32_t channel, const QString &path, QString &err) -> bool {
        return captureChannel(channel, path, err);
      });

  m_pendingRetryTimer->setInterval(kPendingBufRetryIntervalMs);
  connect(m_pendingRetryTimer, &QTimer::timeout, this,
          [this]() { retryPendingBufs(); });

  m_cursorTimer->setInterval(kCursorSampleIntervalMs);
  connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
    QPoint pos = QCursor::pos();
    m_cursorX = pos.x();
    m_cursorY = pos.y();
  });
}

CaptureCoordinator::~CaptureCoordinator() { teardownSockets(); }

void CaptureCoordinator::registerItem(WallpaperCaptureItem *item) {
  if (!item ||
      std::find(m_items.begin(), m_items.end(), item) != m_items.end()) {
    return;
  }
  m_items.push_back(item);
  ensureSocketsBound();
  reevaluateActiveItem();
  retryPendingBufs();
}

void CaptureCoordinator::unregisterItem(WallpaperCaptureItem *item) {
  auto it = std::find(m_items.begin(), m_items.end(), item);
  if (it == m_items.end()) {
    return;
  }
  m_items.erase(it);
  for (auto channelIt = m_channelItems.begin();
       channelIt != m_channelItems.end();) {
    if (channelIt->second == item) {
      channelIt = m_channelItems.erase(channelIt);
    } else {
      ++channelIt;
    }
  }
  if (m_primaryItem == item) {
    m_primaryItem = nullptr;
  }
  reevaluateActiveItem();
  if (m_items.empty()) {
    teardownSockets();
  }
}

void CaptureCoordinator::reevaluateActiveItem() {
  WallpaperCaptureItem *candidate = nullptr;
  QScreen *primary = QGuiApplication::primaryScreen();
  for (auto *item : m_items) {
    if (item->window() && item->window()->screen() == primary) {
      candidate = item;
      break;
    }
  }
  if (!candidate && !m_items.empty()) {
    candidate = m_items.front();
  }
  if (candidate == m_primaryItem) {
    return;
  }
  m_primaryItem = candidate;
  if (m_primaryItem) {
    qInfo() << "[coordinator] primary wallpaper item is now on screen"
            << (m_primaryItem->window() && m_primaryItem->window()->screen()
                    ? m_primaryItem->window()->screen()->name()
                    : QStringLiteral("<none>"));
  }
}

WallpaperCaptureItem *CaptureCoordinator::channelItem(uint32_t channel) const {
  auto it = m_channelItems.find(channel);
  return it == m_channelItems.end() ? nullptr : it->second;
}

WallpaperCaptureItem *
CaptureCoordinator::claimItemByOutputName(uint32_t channel,
                                          const QString &name) {
  for (auto *item : m_items) {
    bool alreadyClaimed = false;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == item) {
        alreadyClaimed = true;
        break;
      }
    }
    if (alreadyClaimed) {
      continue;
    }
    QScreen *screen = item->window() ? item->window()->screen() : nullptr;
    if (screen && screen->name() == name) {
      m_channelItems[channel] = item;
      qInfo() << "[coordinator] channel" << channel
              << "matched by XRandR output name" << name;
      return item;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *CaptureCoordinator::claimItemForSize(uint32_t channel,
                                                           quint32 width,
                                                           quint32 height) {
  if (auto name = x11OutputForSize(width, height)) {
    if (WallpaperCaptureItem *item = claimItemByOutputName(channel, *name)) {
      return item;
    }
  }

  double globalScale = xwaylandGlobalScale();
  for (auto *item : m_items) {
    bool alreadyClaimed = false;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == item) {
        alreadyClaimed = true;
        break;
      }
    }
    if (alreadyClaimed) {
      continue;
    }
    std::optional<WallpiperProtocol::MonitorGeometry> geometry =
        item->currentGeometry();
    if (!geometry) {
      continue;
    }
    WallpiperProtocol::MonitorGeometry predicted =
        predictedXWaylandRect(*geometry, globalScale);
    if (nearlyEqual(predicted.width, width) &&
        nearlyEqual(predicted.height, height)) {
      m_channelItems[channel] = item;
      QScreen *screen = item->window() ? item->window()->screen() : nullptr;
      qInfo() << "[coordinator] channel" << channel << "bound to screen"
              << (screen ? screen->name() : QStringLiteral("<unknown>"))
              << "for stream" << width << "x" << height << "(predicted"
              << predicted.width << "x" << predicted.height
              << "at XWayland scale" << globalScale << ")";
      return item;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *
CaptureCoordinator::claimItemForPosition(uint32_t channel, qint32 x, qint32 y) {
  if (auto name = x11OutputForPosition(x, y)) {
    if (WallpaperCaptureItem *item = claimItemByOutputName(channel, *name)) {
      return item;
    }
  }

  double globalScale = xwaylandGlobalScale();
  for (auto *item : m_items) {
    bool alreadyClaimed = false;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == item) {
        alreadyClaimed = true;
        break;
      }
    }
    if (alreadyClaimed) {
      continue;
    }
    std::optional<WallpiperProtocol::MonitorGeometry> geometry =
        item->currentGeometry();
    if (!geometry) {
      continue;
    }
    WallpiperProtocol::MonitorGeometry predicted =
        predictedXWaylandRect(*geometry, globalScale);
    if (nearlyEqual(predicted.x, x) && nearlyEqual(predicted.y, y)) {
      m_channelItems[channel] = item;
      QScreen *screen = item->window() ? item->window()->screen() : nullptr;
      qInfo() << "[coordinator] channel" << channel << "bound to screen"
              << (screen ? screen->name() : QStringLiteral("<unknown>"))
              << "at position" << x << "," << y << "(predicted" << predicted.x
              << "," << predicted.y << "at XWayland scale" << globalScale
              << ")";
      return item;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *CaptureCoordinator::claimItem(uint32_t channel,
                                                    quint32 width,
                                                    quint32 height,
                                                    bool hasGeometry,
                                                    qint32 geomX,
                                                    qint32 geomY) {
  WallpaperCaptureItem *item = channelItem(channel);
  if (!item && hasGeometry) {
    item = claimItemForPosition(channel, geomX, geomY);
  }
  if (!item) {
    item = claimItemForSize(channel, width, height);
  }
  return item;
}

void CaptureCoordinator::handleBuf(quint32 slot, quint32 width,
                                   quint32 height, quint32 format,
                                   quint32 stride, quint64 modifier,
                                   bool hasGeometry, qint32 geomX,
                                   qint32 geomY, int fd, int syncFd) {
  uint32_t channel = slot / kCaptureSlotCount;
  WallpaperCaptureItem *item =
      claimItem(channel, width, height, hasGeometry, geomX, geomY);
  if (item) {
    item->stageBuf(slot, width, height, format, stride, modifier, fd, syncFd);
    return;
  }

  auto it = m_pendingBufs.find(slot);
  if (it != m_pendingBufs.end()) {
    ::close(it->second.fd);
    if (it->second.syncFd >= 0) {
      ::close(it->second.syncFd);
    }
  }
  m_pendingBufs[slot] = PendingBuf{width,  height,      format,
                                   stride, modifier,    hasGeometry,
                                   geomX,  geomY,       fd,
                                   syncFd};
  qInfo() << "[coordinator] no wallpaper item available yet for new channel"
          << channel << "(" << width << "x" << height
          << "), queued for retry";
}

void CaptureCoordinator::retryPendingBufs() {
  for (auto it = m_pendingBufs.begin(); it != m_pendingBufs.end();) {
    quint32 slot = it->first;
    const PendingBuf &pending = it->second;
    uint32_t channel = slot / kCaptureSlotCount;
    WallpaperCaptureItem *item =
        claimItem(channel, pending.width, pending.height,
                 pending.hasGeometry, pending.geomX, pending.geomY);
    if (!item) {
      ++it;
      continue;
    }
    item->stageBuf(slot, pending.width, pending.height, pending.format,
                   pending.stride, pending.modifier, pending.fd,
                   pending.syncFd);
    it = m_pendingBufs.erase(it);
  }
}

void CaptureCoordinator::ensureSocketsBound() {
  m_captureSocket->start();
  m_ctlListener->start();
  if (!m_cursorTimer->isActive()) {
    m_cursorTimer->start();
  }
  if (!m_pendingRetryTimer->isActive()) {
    m_pendingRetryTimer->start();
  }
}

void CaptureCoordinator::teardownSockets() {
  m_captureSocket->stop();
  m_ctlListener->stop();
  m_cursorTimer->stop();
  m_pendingRetryTimer->stop();
  for (auto &[slot, pending] : m_pendingBufs) {
    ::close(pending.fd);
    if (pending.syncFd >= 0) {
      ::close(pending.syncFd);
    }
  }
  m_pendingBufs.clear();
  m_loggedDroppedChannels.clear();
}

std::optional<WallpiperProtocol::MonitorGeometry>
CaptureCoordinator::geometryFromActiveItem() const {
  if (!m_primaryItem) {
    return std::nullopt;
  }
  return m_primaryItem->currentGeometry();
}

void CaptureCoordinator::handleDetach() {
  for (auto *item : m_items) {
    item->requestDetach();
  }
  qInfo() << "[ctl] detached, released all buffers";
}

bool CaptureCoordinator::captureChannel(uint32_t channel, const QString &path,
                                        QString &err) {
  std::shared_ptr<CaptureCompletion> completion;
  QMetaObject::invokeMethod(
      this,
      [this, channel, &path, &completion]() {
        if (WallpaperCaptureItem *item = channelItem(channel)) {
          completion = item->beginCapture(path);
        }
      },
      Qt::BlockingQueuedConnection);

  if (!completion) {
    err = QStringLiteral("no wallpaper item bound to channel %1").arg(channel);
    return false;
  }

  std::unique_lock<std::mutex> lock(completion->mutex);
  bool signaled = completion->cv.wait_for(lock, std::chrono::seconds(8),
                                          [&] { return completion->done; });
  if (!signaled) {
    err = QStringLiteral("timed out waiting for capture");
    return false;
  }
  err = completion->err;
  return completion->ok;
}

void CaptureCoordinator::handleSetDebug(bool enabled) {
  for (auto *item : m_items) {
    item->setDebugEnabled(enabled);
  }
  qInfo() << "[ctl] debug overlay ->" << enabled;
}

} // namespace WallpiperKde
