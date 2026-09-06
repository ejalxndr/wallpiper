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

#include "protocol.h"

#include <QJsonDocument>

#include <cctype>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace WallpiperProtocol {

QString ctlSocketPath(const QString &portalName) {
  return QStringLiteral("/tmp/wallpiper-portal-%1-ctl.sock").arg(portalName);
}

bool MonitorGeometry::operator==(const MonitorGeometry &other) const {
  return x == other.x && y == other.y && width == other.width &&
         height == other.height && logicalWidth == other.logicalWidth &&
         logicalHeight == other.logicalHeight && scale == other.scale;
}

bool MonitorGeometry::operator!=(const MonitorGeometry &other) const {
  return !(*this == other);
}

QJsonObject toJson(const MonitorGeometry &geometry) {
  QJsonObject object;
  object[QStringLiteral("x")] = geometry.x;
  object[QStringLiteral("y")] = geometry.y;
  object[QStringLiteral("width")] = static_cast<qint64>(geometry.width);
  object[QStringLiteral("height")] = static_cast<qint64>(geometry.height);
  object[QStringLiteral("logical_width")] =
      static_cast<qint64>(geometry.logicalWidth);
  object[QStringLiteral("logical_height")] =
      static_cast<qint64>(geometry.logicalHeight);
  object[QStringLiteral("scale")] = geometry.scale;
  return object;
}

std::optional<MonitorGeometry> geometryFromJson(const QJsonObject &object) {
  if (!object.contains(QStringLiteral("x")) ||
      !object.contains(QStringLiteral("y")) ||
      !object.contains(QStringLiteral("width")) ||
      !object.contains(QStringLiteral("height")) ||
      !object.contains(QStringLiteral("logical_width")) ||
      !object.contains(QStringLiteral("logical_height"))) {
    return std::nullopt;
  }
  MonitorGeometry geometry;
  geometry.x = object[QStringLiteral("x")].toInt();
  geometry.y = object[QStringLiteral("y")].toInt();
  geometry.width =
      static_cast<uint32_t>(object[QStringLiteral("width")].toInt());
  geometry.height =
      static_cast<uint32_t>(object[QStringLiteral("height")].toInt());
  geometry.logicalWidth =
      static_cast<uint32_t>(object[QStringLiteral("logical_width")].toInt());
  geometry.logicalHeight =
      static_cast<uint32_t>(object[QStringLiteral("logical_height")].toInt());
  geometry.scale = object.contains(QStringLiteral("scale"))
                       ? object[QStringLiteral("scale")].toDouble(1.0)
                       : 1.0;
  return geometry;
}

static std::vector<std::string> splitWhitespace(const std::string &s) {
  std::vector<std::string> parts;
  std::istringstream stream(s);
  std::string part;
  while (stream >> part) {
    parts.push_back(part);
  }
  return parts;
}

std::optional<SocketEvent> parseEvent(const std::string &header,
                                      const std::vector<int> &fds) {
  auto parts = splitWhitespace(header);
  if (parts.empty()) {
    return std::nullopt;
  }

  try {
    if (parts[0] == "BUF") {
      if (parts.size() < 7 || fds.empty()) {
        return std::nullopt;
      }
      BufEvent event;
      event.slot = static_cast<uint32_t>(std::stoul(parts[1]));
      event.width = static_cast<uint32_t>(std::stoul(parts[2]));
      event.height = static_cast<uint32_t>(std::stoul(parts[3]));
      event.format = static_cast<uint32_t>(std::stoul(parts[4]));
      event.stride = static_cast<uint32_t>(std::stoul(parts[5]));
      event.modifier = std::stoull(parts[6]);
      if (parts.size() >= 9) {
        event.hasGeometry = true;
        event.geomX = static_cast<int32_t>(std::stol(parts[7]));
        event.geomY = static_cast<int32_t>(std::stol(parts[8]));
      }
      event.fd = fds[0];
      event.syncFd = fds.size() > 1 ? fds[1] : -1;
      return event;
    }
    if (parts[0] == "FRAME") {
      if (parts.size() < 2) {
        return std::nullopt;
      }
      FrameEvent event;
      event.slot = static_cast<uint32_t>(std::stoul(parts[1]));
      event.syncFd = fds.empty() ? -1 : fds[0];
      return event;
    }
    if (parts[0] == "SHM") {
      if (parts.size() < 4 || fds.empty()) {
        return std::nullopt;
      }
      ShmEvent event;
      event.width = static_cast<uint32_t>(std::stoul(parts[1]));
      event.height = static_cast<uint32_t>(std::stoul(parts[2]));
      event.stride = static_cast<uint32_t>(std::stoul(parts[3]));
      event.fd = fds[0];
      return event;
    }
  } catch (const std::exception &) {
    return std::nullopt;
  }

  return std::nullopt;
}

std::optional<RecvResult> recvMsg(int sockFd) {
  char headerBuf[256];
  char cmsgBuf[64];

  iovec iov{};
  iov.iov_base = headerBuf;
  iov.iov_len = sizeof(headerBuf);

  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgBuf;
  msg.msg_controllen = sizeof(cmsgBuf);

  ssize_t n = ::recvmsg(sockFd, &msg, 0);
  if (n < 0) {
    return std::nullopt;
  }

  RecvResult result;
  result.header = std::string(headerBuf, static_cast<size_t>(n));
  while (!result.header.empty() &&
         std::isspace(static_cast<unsigned char>(result.header.back()))) {
    result.header.pop_back();
  }
  size_t start = 0;
  while (start < result.header.size() &&
         std::isspace(static_cast<unsigned char>(result.header[start]))) {
    ++start;
  }
  result.header = result.header.substr(start);

  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
    size_t dataLen = cmsg->cmsg_len - CMSG_LEN(0);
    size_t count = dataLen / sizeof(int);
    const auto *data = reinterpret_cast<const int *>(CMSG_DATA(cmsg));
    for (size_t i = 0; i < count; ++i) {
      int fd = -1;
      std::memcpy(&fd, data + i, sizeof(fd));
      result.fds.push_back(fd);
    }
  }

  return result;
}

int bindUnixDgram(const std::string &path) {
  ::unlink(path.c_str());

  int sockFd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (sockFd < 0) {
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(sockFd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  auto addrLen =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  if (::bind(sockFd, reinterpret_cast<sockaddr *>(&addr), addrLen) != 0) {
    ::close(sockFd);
    return -1;
  }

  return sockFd;
}

int bindUnixStreamListener(const std::string &path, int backlog) {
  ::unlink(path.c_str());

  int sockFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sockFd < 0) {
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(sockFd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  auto addrLen =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  if (::bind(sockFd, reinterpret_cast<sockaddr *>(&addr), addrLen) != 0) {
    ::close(sockFd);
    return -1;
  }
  if (::listen(sockFd, backlog) != 0) {
    ::close(sockFd);
    return -1;
  }

  return sockFd;
}

std::optional<CtlRequest> parseCtlRequest(const std::string &line) {
  std::string trimmed = line;
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.pop_back();
  }
  size_t start = 0;
  while (start < trimmed.size() &&
         std::isspace(static_cast<unsigned char>(trimmed[start]))) {
    ++start;
  }
  trimmed = trimmed.substr(start);

  if (trimmed == "GEOMETRY")
    return CtlRequest::Geometry;
  if (trimmed == "DETACH")
    return CtlRequest::Detach;
  if (trimmed == "DEBUG_ON")
    return CtlRequest::DebugOn;
  if (trimmed == "DEBUG_OFF")
    return CtlRequest::DebugOff;
  if (trimmed == "CURSOR_POS")
    return CtlRequest::CursorPos;
  if (trimmed == "PING")
    return CtlRequest::Ping;
  if (trimmed == "RENDER_NODE")
    return CtlRequest::RenderNode;
  if (trimmed.rfind("CAPTURE ", 0) == 0)
    return CtlRequest::Capture;
  return std::nullopt;
}

std::optional<CtlRequestCapture> parseCtlRequestCapture(const std::string &line) {
  std::string trimmed = line;
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.pop_back();
  }
  if (trimmed.rfind("CAPTURE ", 0) != 0) {
    return std::nullopt;
  }
  std::istringstream stream(trimmed.substr(8));
  std::string channelStr;
  if (!(stream >> channelStr)) {
    return std::nullopt;
  }
  std::string rest;
  std::getline(stream, rest);
  size_t start = 0;
  while (start < rest.size() &&
         std::isspace(static_cast<unsigned char>(rest[start]))) {
    ++start;
  }
  rest = rest.substr(start);
  if (rest.empty()) {
    return std::nullopt;
  }

  try {
    CtlRequestCapture result;
    result.channel = static_cast<uint32_t>(std::stoul(channelStr));
    result.path = QString::fromStdString(rest);
    return result;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::string encodeCtlRequest(CtlRequest request) {
  switch (request) {
  case CtlRequest::Geometry:
    return "GEOMETRY\n";
  case CtlRequest::Detach:
    return "DETACH\n";
  case CtlRequest::DebugOn:
    return "DEBUG_ON\n";
  case CtlRequest::DebugOff:
    return "DEBUG_OFF\n";
  case CtlRequest::CursorPos:
    return "CURSOR_POS\n";
  case CtlRequest::Ping:
    return "PING\n";
  case CtlRequest::RenderNode:
    return "RENDER_NODE\n";
  case CtlRequest::Capture:
    return {};
  }
  return {};
}

std::string encodeCtlResponse(const CtlResponse &response) {
  if (std::holds_alternative<CtlResponseOk>(response)) {
    return "OK\n";
  }
  if (const auto *err = std::get_if<CtlResponseErr>(&response)) {
    return "ERR " + err->message + "\n";
  }
  if (const auto *geometry = std::get_if<CtlResponseGeometry>(&response)) {
    QJsonDocument doc(toJson(geometry->geometry));
    return "GEOMETRY " + doc.toJson(QJsonDocument::Compact).toStdString() +
           "\n";
  }
  if (const auto *cursor = std::get_if<CtlResponseCursorPos>(&response)) {
    return "CURSOR_POS " + std::to_string(cursor->x) + " " +
           std::to_string(cursor->y) + "\n";
  }
  if (const auto *node = std::get_if<CtlResponseRenderNode>(&response)) {
    return "RENDER_NODE " + std::to_string(node->major) + " " +
           std::to_string(node->minor) + "\n";
  }
  return {};
}

} // namespace WallpiperProtocol
