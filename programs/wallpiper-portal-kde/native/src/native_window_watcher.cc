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

#include "native_window_watcher.h"

#include "capture_coordinator.h"

#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/event_queue.h>
#include <KWayland/Client/plasmawindowmanagement.h>
#include <KWayland/Client/registry.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace WallpiperKde {

namespace {

constexpr int kSettleIntervalMs = 50;
constexpr int kPollIntervalMs = 1000;
constexpr int kCoveragePercent = 90;

bool isIgnoredApp(const QString &appId) {
  static const QSet<QString> ignored = {
      QStringLiteral("org.kde.plasmashell"),
      QStringLiteral("org.kde.kwin"),
      QStringLiteral("kwin_wayland"),
      QStringLiteral("org.kde.kglobalaccel"),
      QStringLiteral("org.kde.plasma.kicker"),
      QStringLiteral("org.kde.plasma.kickerdash"),
      QStringLiteral("org.kde.latte-dock"),
      QStringLiteral("latte-dock"),
      QStringLiteral("plank"),
  };
  return ignored.contains(appId);
}

bool coversRect(const QRect &window, const QRect &target) {
  if (window.isEmpty() || target.isEmpty() || !window.isValid() ||
      !target.isValid()) {
    return false;
  }

  const QRect overlap = window.intersected(target);
  if (overlap.isEmpty()) {
    return false;
  }

  const qint64 overlapArea =
      static_cast<qint64>(overlap.width()) * overlap.height();
  const qint64 targetArea =
      static_cast<qint64>(target.width()) * target.height();
  if (targetArea <= 0) {
    return false;
  }
  return overlapArea * 100 >= targetArea * kCoveragePercent;
}

void setChannelBit(uint32_t &mask, uint32_t channel) {
  if (channel < 32) {
    mask |= (uint32_t{1} << channel);
  }
}

} // namespace

NativeWindowWatcher::NativeWindowWatcher(CaptureCoordinator *coordinator,
                                         QObject *parent)
    : QObject(parent), m_coordinator(coordinator) {
  QString tempDir = QString::fromLocal8Bit(qgetenv("WALLPIPER_TEMP_DIR"));
  if (tempDir.isEmpty()) {
    tempDir = QStringLiteral("/tmp/wallpiper");
  }
  m_statePath = tempDir + QStringLiteral("/wallpiper-native-window");

  QDir().mkpath(tempDir);

  m_settleTimer = new QTimer(this);
  m_settleTimer->setSingleShot(true);
  m_settleTimer->setInterval(kSettleIntervalMs);
  connect(m_settleTimer, &QTimer::timeout, this,
          &NativeWindowWatcher::evaluate);

  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(kPollIntervalMs);
  connect(m_pollTimer, &QTimer::timeout, this,
          &NativeWindowWatcher::evaluate);

  QMetaObject::invokeMethod(this, &NativeWindowWatcher::start,
                            Qt::QueuedConnection);
}

NativeWindowWatcher::~NativeWindowWatcher() = default;

void NativeWindowWatcher::start() {
  if (m_started) {
    return;
  }
  m_started = true;

  if (QGuiApplication::platformName() != QStringLiteral("wayland") ||
      qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
    return;
  }

  m_connection = new KWayland::Client::ConnectionThread(this);
  m_queue = new KWayland::Client::EventQueue(this);
  m_registry = new KWayland::Client::Registry(this);

  connect(m_connection, &KWayland::Client::ConnectionThread::connected, this,
          &NativeWindowWatcher::bindWindowManager);
  connect(m_connection, &KWayland::Client::ConnectionThread::connectionDied,
          this, [this] {
            m_windows.clear();
            m_windowManager = nullptr;
            writeMasks(0, 0);
            qWarning() << "[native-window] Wayland connection died";
          });
  connect(m_connection, &KWayland::Client::ConnectionThread::failed, this, [] {
    qWarning() << "[native-window] failed to connect to Wayland";
  });

  m_connection->initConnection();
}

void NativeWindowWatcher::bindWindowManager() {
  qInfo() << "[native-window] connected to Wayland";

  m_queue->setup(m_connection);
  m_registry->setEventQueue(m_queue);
  m_registry->create(m_connection);
  m_registry->setup();

  connect(m_registry,
          &KWayland::Client::Registry::plasmaWindowManagementAnnounced, this,
          [this](quint32 name, quint32 version) {
            if (m_windowManager) {
              return;
            }
            m_windowManager = m_registry->createPlasmaWindowManagement(
                name, version, m_registry);
            if (!m_windowManager) {
              qWarning() << "[native-window] could not bind Plasma window "
                            "management";
              return;
            }

            connect(m_windowManager, &QObject::destroyed, this,
                    [this] { m_windowManager = nullptr; });
            connect(m_windowManager,
                    &KWayland::Client::PlasmaWindowManagement::windowCreated,
                    this, &NativeWindowWatcher::watchWindow);

            const QList<KWayland::Client::PlasmaWindow *> windows =
                m_windowManager->windows();
            for (KWayland::Client::PlasmaWindow *window : windows) {
              watchWindow(window);
            }

            m_pollTimer->start();
            qInfo() << "[native-window] watching" << windows.size()
                    << "existing windows";
            evaluate();
          });
}

void NativeWindowWatcher::watchWindow(
    KWayland::Client::PlasmaWindow *window) {
  if (!window || m_windows.contains(window)) {
    return;
  }
  m_windows.insert(window);

  connect(window, &QObject::destroyed, this,
          [this, window] { m_windows.remove(window); });
  connect(window, &KWayland::Client::PlasmaWindow::unmapped, this,
          [this, window] {
            m_windows.remove(window);
            m_settleTimer->start();
          });
  connect(window, &KWayland::Client::PlasmaWindow::fullscreenChanged, this,
          [this] { m_settleTimer->start(); });
  connect(window, &KWayland::Client::PlasmaWindow::maximizedChanged, this,
          [this] { m_settleTimer->start(); });
  connect(window, &KWayland::Client::PlasmaWindow::minimizedChanged, this,
          [this] { m_settleTimer->start(); });
  connect(window, &KWayland::Client::PlasmaWindow::geometryChanged, this,
          [this] { m_settleTimer->start(); });

  m_settleTimer->start();
}

void NativeWindowWatcher::evaluate() {
  uint32_t fullscreenMask = 0;
  uint32_t maximizedMask = 0;

  const QList<QScreen *> screens = QGuiApplication::screens();
  for (KWayland::Client::PlasmaWindow *window : m_windows) {
    if (!window || window->isMinimized() || isIgnoredApp(window->appId())) {
      continue;
    }

    const QRect windowRect = window->geometry();
    if (windowRect.isEmpty()) {
      continue;
    }

    bool fullscreen = window->isFullscreen();
    bool maximized = window->isMaximized();

    // Wallpaper Engine treats a non-maximized window that covers a monitor's
    // work area as fullscreen.  Mirror that when KWin has not flagged it.
    if (!fullscreen && !maximized) {
      for (QScreen *screen : screens) {
        if (coversRect(windowRect, screen->availableGeometry())) {
          fullscreen = true;
          break;
        }
      }
    }

    if (!fullscreen && !maximized) {
      continue;
    }

    for (QScreen *screen : screens) {
      const QRect target = fullscreen ? screen->geometry()
                                      : screen->availableGeometry();
      if (!coversRect(windowRect, target)) {
        continue;
      }
      const std::optional<uint32_t> channel =
          m_coordinator->channelForScreen(screen);
      if (!channel) {
        continue;
      }
      if (fullscreen) {
        setChannelBit(fullscreenMask, *channel);
      }
      if (maximized) {
        setChannelBit(maximizedMask, *channel);
      }
    }
  }

  writeMasks(fullscreenMask, maximizedMask);
}

void NativeWindowWatcher::writeMasks(uint32_t fullscreenMask,
                                     uint32_t maximizedMask) {
  if (m_haveWritten && fullscreenMask == m_lastFullscreenMask &&
      maximizedMask == m_lastMaximizedMask) {
    return;
  }

  unsigned char payload[8];
  std::memcpy(payload, &fullscreenMask, sizeof(fullscreenMask));
  std::memcpy(payload + sizeof(fullscreenMask), &maximizedMask,
              sizeof(maximizedMask));

  /*
   * Write the fixed-size payload directly instead of publishing through
   * rename(2).  The Wine shim keeps this path open while polling it, and on
   * this Plasma/XWayland setup rename over the live file fails with EPERM.
   * An 8-byte single write is enough here: the reader discards short reads.
   */
  QFile out(m_statePath);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      out.write(reinterpret_cast<const char *>(payload), sizeof(payload)) !=
          static_cast<qint64>(sizeof(payload))) {
    if (!m_stateWriteWarningLogged) {
      m_stateWriteWarningLogged = true;
      qWarning() << "[native-window] cannot write" << m_statePath
                 << out.errorString();
    }
    return;
  }
  out.close();

  m_lastFullscreenMask = fullscreenMask;
  m_lastMaximizedMask = maximizedMask;
  m_haveWritten = true;
  qInfo() << "[native-window] masks fs=" << fullscreenMask
          << "max=" << maximizedMask;
}

} // namespace WallpiperKde
