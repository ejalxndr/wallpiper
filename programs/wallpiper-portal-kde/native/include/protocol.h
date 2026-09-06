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

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace WallpiperProtocol {

constexpr const char *kCaptureSocketPath = "/tmp/wallpiper-capture.sock";

QString ctlSocketPath(const QString &portalName);

struct MonitorGeometry {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t logicalWidth = 0;
  uint32_t logicalHeight = 0;
  double scale = 1.0;

  bool operator==(const MonitorGeometry &other) const;
  bool operator!=(const MonitorGeometry &other) const;
};

QJsonObject toJson(const MonitorGeometry &geometry);
std::optional<MonitorGeometry> geometryFromJson(const QJsonObject &object);

struct BufEvent {
  uint32_t slot = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t stride = 0;
  uint64_t modifier = 0;
  bool hasGeometry = false;
  int32_t geomX = 0;
  int32_t geomY = 0;
  int fd = -1;
  int syncFd = -1;
};

struct FrameEvent {
  uint32_t slot = 0;
  int syncFd = -1;
};

struct ShmEvent {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  int fd = -1;
};

using SocketEvent = std::variant<BufEvent, FrameEvent, ShmEvent>;

std::optional<SocketEvent> parseEvent(const std::string &header,
                                      const std::vector<int> &fds);

struct RecvResult {
  std::string header;
  std::vector<int> fds;
};

std::optional<RecvResult> recvMsg(int sockFd);

int bindUnixDgram(const std::string &path);

int bindUnixStreamListener(const std::string &path, int backlog = 8);

enum class CtlRequest {
  Geometry,
  Detach,
  DebugOn,
  DebugOff,
  CursorPos,
  Ping,
  Capture,
  RenderNode,
};

std::optional<CtlRequest> parseCtlRequest(const std::string &line);
std::string encodeCtlRequest(CtlRequest request);

struct CtlRequestCapture {
  uint32_t channel = 0;
  QString path;
};
/* Only valid to call when parseCtlRequest() returned CtlRequest::Capture
 * for the same line. */
std::optional<CtlRequestCapture> parseCtlRequestCapture(const std::string &line);

struct CtlResponseOk {};
struct CtlResponseErr {
  std::string message;
};
struct CtlResponseGeometry {
  MonitorGeometry geometry;
};
struct CtlResponseCursorPos {
  int32_t x = 0;
  int32_t y = 0;
};
struct CtlResponseRenderNode {
  uint32_t major = 0;
  uint32_t minor = 0;
};

using CtlResponse =
    std::variant<CtlResponseOk, CtlResponseErr, CtlResponseGeometry,
                CtlResponseCursorPos, CtlResponseRenderNode>;
std::string encodeCtlResponse(const CtlResponse &response);
} // namespace WallpiperProtocol
