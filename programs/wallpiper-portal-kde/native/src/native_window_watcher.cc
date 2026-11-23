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

#include "audio_activity_watcher.h"
#include "capture_coordinator.h"
#include "wallpiper/native_window_state.h"

#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/event_queue.h>
#include <KWayland/Client/plasmawindowmanagement.h>
#include <KWayland/Client/registry.h>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QScreen>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <optional>

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
  return ignored.contains(appId) ||
         appId.contains(QStringLiteral("wallpaper"), Qt::CaseInsensitive);
}

qint64 overlapArea(const QRect &a, const QRect &b) {
  if (a.isEmpty() || b.isEmpty() || !a.isValid() || !b.isValid()) {
    return 0;
  }
  const QRect overlap = a.intersected(b);
  if (overlap.isEmpty()) {
    return 0;
  }
  return static_cast<qint64>(overlap.width()) * overlap.height();
}

bool coversRect(const QRect &window, const QRect &target) {
  if (target.isEmpty() || !target.isValid()) {
    return false;
  }
  const qint64 targetArea =
      static_cast<qint64>(target.width()) * target.height();
  if (targetArea <= 0) {
    return false;
  }
  return overlapArea(window, target) * 100 >= targetArea * kCoveragePercent;
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
  connect(m_pollTimer, &QTimer::timeout, this, &NativeWindowWatcher::evaluate);

  m_audioWatcher = new AudioActivityWatcher;

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.connect(QStringLiteral("org.freedesktop.ScreenSaver"),
                   QStringLiteral("/ScreenSaver"),
                   QStringLiteral("org.freedesktop.ScreenSaver"),
                   QStringLiteral("ActiveChanged"), this,
                   SLOT(screenSaverActiveChanged(bool)))) {
    qWarning()
        << "[native-window] cannot subscribe to ScreenSaver.ActiveChanged";
  }

  QDBusInterface screensaver(QStringLiteral("org.freedesktop.ScreenSaver"),
                             QStringLiteral("/ScreenSaver"),
                             QStringLiteral("org.freedesktop.ScreenSaver"), bus,
                             this);
  if (screensaver.isValid()) {
    const QDBusReply<bool> reply =
        screensaver.call(QStringLiteral("GetActive"));
    if (reply.isValid()) {
      m_screenOff = reply.value();
    } else {
      qWarning() << "[native-window] ScreenSaver.GetActive failed:"
                 << reply.error().message();
    }
  }

  m_pollTimer->start();

  QMetaObject::invokeMethod(this, &NativeWindowWatcher::start,
                            Qt::QueuedConnection);
}

NativeWindowWatcher::~NativeWindowWatcher() { delete m_audioWatcher; }

void NativeWindowWatcher::screenSaverActiveChanged(bool active) {
  if (m_screenOff == active) {
    return;
  }
  m_screenOff = active;
  evaluate();
}

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
            qWarning() << "[native-window] Wayland connection died";
            evaluate();
          });
  connect(m_connection, &KWayland::Client::ConnectionThread::failed, this,
          [] { qWarning() << "[native-window] failed to connect to Wayland"; });

  m_connection->initConnection();
}

void NativeWindowWatcher::bindWindowManager() {
  qInfo() << "[native-window] connected to Wayland";

  m_queue->setup(m_connection);
  m_registry->setEventQueue(m_queue);
  m_registry->create(m_connection);
  m_registry->setup();

  connect(
      m_registry, &KWayland::Client::Registry::plasmaWindowManagementAnnounced,
      this, [this](quint32 name, quint32 version) {
        if (m_windowManager) {
          return;
        }
        m_windowManager =
            m_registry->createPlasmaWindowManagement(name, version, m_registry);
        if (!m_windowManager) {
          qWarning() << "[native-window] could not bind Plasma window "
                        "management";
          return;
        }

        connect(m_windowManager, &QObject::destroyed, this,
                [this] { m_windowManager = nullptr; });
        connect(m_windowManager,
                &KWayland::Client::PlasmaWindowManagement::windowCreated, this,
                &NativeWindowWatcher::watchWindow);

        const QList<KWayland::Client::PlasmaWindow *> windows =
            m_windowManager->windows();
        for (KWayland::Client::PlasmaWindow *window : windows) {
          watchWindow(window);
        }

        qInfo() << "[native-window] watching" << windows.size()
                << "existing windows";
        evaluate();
      });
}

void NativeWindowWatcher::watchWindow(KWayland::Client::PlasmaWindow *window) {
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
  connect(window, &KWayland::Client::PlasmaWindow::activeChanged, this,
          [this] { m_settleTimer->start(); });
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
  struct ScreenStateBuilder {
    uint32_t flags = 0;
    uint32_t channel = 0;
  };
  QHash<const QScreen *, ScreenStateBuilder> statesByScreen;

  const QList<QScreen *> screens = QGuiApplication::screens();

  auto screenHasChannel =
      [this](const QScreen *screen) -> std::optional<uint32_t> {
    if (!m_coordinator) {
      return std::nullopt;
    }
    const std::optional<uint32_t> channel =
        m_coordinator->channelForScreen(screen);
    if (!channel || *channel >= 32) {
      return std::nullopt;
    }
    return channel;
  };

  KWayland::Client::PlasmaWindow *activeWindow = nullptr;
  for (KWayland::Client::PlasmaWindow *window : m_windows) {
    if (!window || !window->isActive() || window->isMinimized() ||
        isIgnoredApp(window->appId())) {
      continue;
    }
    activeWindow = window;
    break;
  }

  if (activeWindow) {
    const QRect windowRect = activeWindow->geometry();
    QScreen *focusScreen = nullptr;
    qint64 bestArea = 0;
    for (QScreen *screen : screens) {
      const qint64 area = overlapArea(windowRect, screen->geometry());
      if (area > bestArea) {
        bestArea = area;
        focusScreen = screen;
      }
    }
    if (!focusScreen && !windowRect.isEmpty()) {
      const QPoint center = windowRect.center();
      for (QScreen *screen : screens) {
        if (screen->geometry().contains(center)) {
          focusScreen = screen;
          break;
        }
      }
    }

    if (focusScreen) {
      const std::optional<uint32_t> channel = screenHasChannel(focusScreen);
      if (channel) {
        ScreenStateBuilder &entry = statesByScreen[focusScreen];
        entry.channel = *channel;
        entry.flags |= WP_NATIVE_WINDOW_FLAG_FOCUSED;
      }
    }
  }

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
      const QRect target =
          fullscreen ? screen->geometry() : screen->availableGeometry();
      if (!coversRect(windowRect, target)) {
        continue;
      }
      // Only screens with a bound capture channel are managed by wallpiper.
      const std::optional<uint32_t> channel = screenHasChannel(screen);
      if (!channel) {
        continue;
      }
      ScreenStateBuilder &entry = statesByScreen[screen];
      entry.channel = *channel;
      if (fullscreen) {
        entry.flags |= WP_NATIVE_WINDOW_FLAG_FULLSCREEN;
      }
      if (maximized) {
        entry.flags |= WP_NATIVE_WINDOW_FLAG_MAXIMIZED;
      }
    }
  }

  QVector<ScreenState> states;
  states.reserve(statesByScreen.size());
  for (auto it = statesByScreen.cbegin(); it != statesByScreen.cend(); ++it) {
    const QRect geometry = it.key()->geometry();
    states.push_back(ScreenState{geometry.x(), geometry.y(), geometry.width(),
                                 geometry.height(), it.value().flags,
                                 it.value().channel});
  }
  writeState(states);
}

void NativeWindowWatcher::writeState(const QVector<ScreenState> &states) {
  static_assert(sizeof(int32_t) == sizeof(int),
                "state file layout requires 32-bit int");
  static_assert(WP_NATIVE_WINDOW_STATE_V1_SIZE == 176u,
                "WNW1 wire layout changed");
  static_assert(sizeof(WpNativeWindowState) == 184u,
                "WNW2 wire layout changed");

  WpNativeWindowState payload;
  std::memset(&payload, 0, sizeof(payload));
  payload.magic = WP_NATIVE_WINDOW_STATE_MAGIC;
  payload.audio_active = m_audioWatcher && m_audioWatcher->active() ? 1u : 0u;
  payload.screen_off = m_screenOff ? 1u : 0u;
  payload.count =
      std::min(static_cast<uint32_t>(states.size()),
               static_cast<uint32_t>(WP_NATIVE_WINDOW_STATE_MAX_SCREENS));
  for (uint32_t i = 0; i < payload.count; ++i) {
    payload.screens[i].x = states[static_cast<int>(i)].x;
    payload.screens[i].y = states[static_cast<int>(i)].y;
    payload.screens[i].width = states[static_cast<int>(i)].width;
    payload.screens[i].height = states[static_cast<int>(i)].height;
    payload.screens[i].flags = states[static_cast<int>(i)].flags;

    const uint32_t channel = states[static_cast<int>(i)].channel;
    if (channel < 32) {
      if (payload.screens[i].flags & WP_NATIVE_WINDOW_FLAG_FULLSCREEN) {
        payload.legacy_fullscreen_mask |= uint32_t{1} << channel;
      }
      if (payload.screens[i].flags & WP_NATIVE_WINDOW_FLAG_MAXIMIZED) {
        payload.legacy_maximized_mask |= uint32_t{1} << channel;
      }
    }
  }

  const QByteArray bytes(reinterpret_cast<const char *>(&payload),
                         sizeof(payload));
  if (m_haveWritten && bytes == m_lastPayload) {
    return;
  }

  /*
   * Write the fixed-size payload directly instead of publishing through
   * rename(2).  The Wine shim keeps this path open while polling it, and on
   * this Plasma/XWayland setup rename over the live file fails with EPERM.
   * A single write of the whole struct is enough: the reader discards shorts.
   */
  QFile out(m_statePath);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      out.write(bytes) != bytes.size()) {
    if (!m_stateWriteWarningLogged) {
      m_stateWriteWarningLogged = true;
      qWarning() << "[native-window] cannot write" << m_statePath
                 << out.errorString();
    }
    return;
  }
  out.close();

  m_lastPayload = bytes;
  m_haveWritten = true;
  qInfo() << "[native-window] published" << payload.count
          << "screen states, audio=" << payload.audio_active
          << "screen_off=" << payload.screen_off << ":" << bytes.toHex();
}

} // namespace WallpiperKde
