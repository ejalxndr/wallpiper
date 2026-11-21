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

#include <QObject>
#include <QSet>
#include <QString>

#include <cstdint>

class QTimer;

namespace KWayland {
namespace Client {
class ConnectionThread;
class EventQueue;
class PlasmaWindow;
class PlasmaWindowManagement;
class Registry;
} // namespace Client
} // namespace KWayland

namespace WallpiperKde {

class CaptureCoordinator;

/*
 * Watches native Plasma windows through KWin's Plasma window-management
 * protocol and publishes per-capture-channel fullscreen/maximized masks into
 * the wallpiper temp directory.  The COFF shim loaded into wallpaper64.exe
 * ORs these masks into Wallpaper Engine's own window-state masks, which makes
 * WE's existing per-monitor pause settings work for native Wayland windows.
 */
class NativeWindowWatcher : public QObject {
  Q_OBJECT
public:
  explicit NativeWindowWatcher(CaptureCoordinator *coordinator,
                               QObject *parent = nullptr);
  ~NativeWindowWatcher() override;

private slots:
  void start();
  void evaluate();

private:
  void bindWindowManager();
  void watchWindow(KWayland::Client::PlasmaWindow *window);
  void writeMasks(uint32_t fullscreenMask, uint32_t maximizedMask);

  CaptureCoordinator *m_coordinator = nullptr;
  KWayland::Client::ConnectionThread *m_connection = nullptr;
  KWayland::Client::EventQueue *m_queue = nullptr;
  KWayland::Client::Registry *m_registry = nullptr;
  KWayland::Client::PlasmaWindowManagement *m_windowManager = nullptr;
  QSet<KWayland::Client::PlasmaWindow *> m_windows;
  QTimer *m_settleTimer = nullptr;
  QTimer *m_pollTimer = nullptr;
  QString m_statePath;
  uint32_t m_lastFullscreenMask = 0;
  uint32_t m_lastMaximizedMask = 0;
  bool m_haveWritten = false;
  bool m_stateWriteWarningLogged = false;
  bool m_started = false;
};

} // namespace WallpiperKde
