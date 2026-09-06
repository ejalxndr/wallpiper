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

#include "ctl_listener.h"

#include <QDebug>

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace WallpiperKde {

CtlListener::CtlListener(QString portalName, QObject *parent)
    : QObject(parent), m_portalName(std::move(portalName)) {}

CtlListener::~CtlListener() { stop(); }

void CtlListener::start() {
  if (m_running.exchange(true)) {
    return;
  }
  m_thread = std::thread(&CtlListener::run, this);
}

void CtlListener::stop() {
  if (!m_running.exchange(false)) {
    return;
  }
  int fd = m_listenFd.exchange(-1);
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void CtlListener::setCursorPosProvider(CursorPosProvider provider) {
  m_cursorPosProvider = std::move(provider);
}

void CtlListener::setGeometryProvider(GeometryProvider provider) {
  m_geometryProvider = std::move(provider);
}

void CtlListener::setDetachHandler(DetachHandler handler) {
  m_detachHandler = std::move(handler);
}

void CtlListener::setDebugHandler(DebugHandler handler) {
  m_debugHandler = std::move(handler);
}

void CtlListener::setCaptureHandler(CaptureHandler handler) {
  m_captureHandler = std::move(handler);
}

void CtlListener::setRenderNodeProvider(RenderNodeProvider provider) {
  m_renderNodeProvider = std::move(provider);
}

void CtlListener::run() {
  const std::string path =
      WallpiperProtocol::ctlSocketPath(m_portalName).toStdString();
  int listenFd = WallpiperProtocol::bindUnixStreamListener(path);
  if (listenFd < 0) {
    qWarning() << "[ctl] failed to bind" << QString::fromStdString(path) << ":"
               << std::strerror(errno);
    m_running = false;
    return;
  }
  m_listenFd = listenFd;
  qInfo() << "[ctl] listening on" << QString::fromStdString(path);

  while (m_running.load()) {
    int clientFd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (clientFd < 0) {
      continue;
    }
    handleConnection(clientFd);
    ::close(clientFd);
  }
}

void CtlListener::handleConnection(int clientFd) {
  std::string line;
  char ch = '\0';
  while (::read(clientFd, &ch, 1) == 1) {
    if (ch == '\n') {
      break;
    }
    line.push_back(ch);
    if (line.size() > 256) {
      break;
    }
  }
  if (line.empty()) {
    return;
  }

  auto writeResponse = [clientFd](
                           const WallpiperProtocol::CtlResponse &response) {
    std::string encoded = WallpiperProtocol::encodeCtlResponse(response);
    size_t written = 0;
    while (written < encoded.size()) {
      ssize_t n =
          ::write(clientFd, encoded.data() + written, encoded.size() - written);
      if (n <= 0) {
        return;
      }
      written += static_cast<size_t>(n);
    }
  };

  auto request = WallpiperProtocol::parseCtlRequest(line);
  if (!request) {
    writeResponse(WallpiperProtocol::CtlResponseErr{"unrecognized command"});
    return;
  }

  if (*request == WallpiperProtocol::CtlRequest::CursorPos) {
    WallpiperProtocol::CtlResponse response =
        WallpiperProtocol::CtlResponseErr{"cursor position unavailable"};
    if (m_cursorPosProvider) {
      if (auto pos = m_cursorPosProvider()) {
        response =
            WallpiperProtocol::CtlResponseCursorPos{pos->first, pos->second};
      }
    }
    writeResponse(response);
    return;
  }

  WallpiperProtocol::CtlResponse response =
      WallpiperProtocol::CtlResponseErr{"listener shutting down"};
  switch (*request) {
  case WallpiperProtocol::CtlRequest::Geometry:
    if (m_geometryProvider) {
      if (auto geometry = m_geometryProvider()) {
        response = WallpiperProtocol::CtlResponseGeometry{*geometry};
      } else {
        response = WallpiperProtocol::CtlResponseErr{"geometry unavailable"};
      }
    }
    break;
  case WallpiperProtocol::CtlRequest::Detach:
    if (m_detachHandler) {
      m_detachHandler();
      response = WallpiperProtocol::CtlResponseOk{};
    }
    break;
  case WallpiperProtocol::CtlRequest::DebugOn:
    if (m_debugHandler) {
      m_debugHandler(true);
      response = WallpiperProtocol::CtlResponseOk{};
    }
    break;
  case WallpiperProtocol::CtlRequest::DebugOff:
    if (m_debugHandler) {
      m_debugHandler(false);
      response = WallpiperProtocol::CtlResponseOk{};
    }
    break;
  case WallpiperProtocol::CtlRequest::CursorPos:
    break;
  case WallpiperProtocol::CtlRequest::Ping:
    response = WallpiperProtocol::CtlResponseOk{};
    break;
  case WallpiperProtocol::CtlRequest::Capture: {
    auto args = WallpiperProtocol::parseCtlRequestCapture(line);
    if (!args) {
      response = WallpiperProtocol::CtlResponseErr{"malformed capture request"};
      break;
    }
    if (!m_captureHandler) {
      response = WallpiperProtocol::CtlResponseErr{"capture unavailable"};
      break;
    }
    QString err;
    if (m_captureHandler(args->channel, args->path, err)) {
      response = WallpiperProtocol::CtlResponseOk{};
    } else {
      response = WallpiperProtocol::CtlResponseErr{err.toStdString()};
    }
    break;
  }
  case WallpiperProtocol::CtlRequest::RenderNode:
    if (m_renderNodeProvider) {
      if (auto node = m_renderNodeProvider()) {
        response =
            WallpiperProtocol::CtlResponseRenderNode{node->first, node->second};
      } else {
        response = WallpiperProtocol::CtlResponseErr{"render node unavailable"};
      }
    }
    break;
  }
  writeResponse(response);
}

} // namespace WallpiperKde
