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
#include <QString>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

namespace WallpiperKde {

class CtlListener : public QObject {
  Q_OBJECT
public:
  using CursorPosProvider =
      std::function<std::optional<std::pair<int32_t, int32_t>>()>;
  using GeometryProvider =
      std::function<std::optional<WallpiperProtocol::MonitorGeometry>()>;
  using DetachHandler = std::function<void()>;
  using DebugHandler = std::function<void(bool)>;
  using CaptureHandler =
      std::function<bool(uint32_t channel, const QString &path, QString &err)>;
  using RenderNodeProvider =
      std::function<std::optional<std::pair<uint32_t, uint32_t>>()>;

  explicit CtlListener(QString portalName, QObject *parent = nullptr);
  ~CtlListener() override;

  void start();
  void stop();

  void setCursorPosProvider(CursorPosProvider provider);
  void setGeometryProvider(GeometryProvider provider);
  void setDetachHandler(DetachHandler handler);
  void setDebugHandler(DebugHandler handler);
  void setCaptureHandler(CaptureHandler handler);
  void setRenderNodeProvider(RenderNodeProvider provider);

private:
  void run();
  void handleConnection(int clientFd);

  QString m_portalName;
  std::thread m_thread;
  std::atomic<int> m_listenFd{-1};
  std::atomic<bool> m_running{false};

  CursorPosProvider m_cursorPosProvider;
  GeometryProvider m_geometryProvider;
  DetachHandler m_detachHandler;
  DebugHandler m_debugHandler;
  CaptureHandler m_captureHandler;
  RenderNodeProvider m_renderNodeProvider;
};

} // namespace WallpiperKde
