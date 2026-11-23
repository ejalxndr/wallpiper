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

#include "audio_activity_watcher.h"

#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/operation.h>
#include <pulse/proplist.h>

#include <QString>

#include <chrono>
#include <cstdio>

namespace WallpiperKde {

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(2);
constexpr auto kScanInterval = std::chrono::milliseconds(250);
constexpr auto kIterateInterval = std::chrono::milliseconds(20);

struct ScanState {
  AudioActivityWatcher *watcher;
  bool any = false;
};

bool isWallpaperEngine(const pa_sink_input_info *info) {
  if (!info->proplist) {
    return false;
  }

  const char *appName =
      pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
  const char *binary =
      pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);

  if (appName && QString::fromUtf8(appName).contains(
                     QStringLiteral("wallpaper engine"), Qt::CaseInsensitive)) {
    return true;
  }
  if (binary && QString::fromUtf8(binary).contains(
                    QStringLiteral("wallpaper64"), Qt::CaseInsensitive)) {
    return true;
  }
  return false;
}

void sinkInputCallback(pa_context *, const pa_sink_input_info *info, int eol,
                       void *userdata) {
  ScanState *scan = static_cast<ScanState *>(userdata);
  if (!scan) {
    return;
  }
  if (eol) {
    scan->watcher->setActive(scan->any);
    return;
  }
  if (!info || info->corked || info->mute || isWallpaperEngine(info)) {
    return;
  }
  scan->any = true;
}

} // namespace

AudioActivityWatcher::AudioActivityWatcher() {
  m_thread = std::thread(&AudioActivityWatcher::run, this);
}

AudioActivityWatcher::~AudioActivityWatcher() {
  m_stop.store(true, std::memory_order_release);
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void AudioActivityWatcher::run() {
  pa_mainloop *loop = pa_mainloop_new();
  if (!loop) {
    return;
  }

  pa_context *context =
      pa_context_new(pa_mainloop_get_api(loop), "wallpiper-kde-audio");
  if (!context) {
    pa_mainloop_free(loop);
    return;
  }

  pa_context_connect(context, nullptr, PA_CONTEXT_NOFAIL, nullptr);

  const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
  while (!m_stop.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline &&
         pa_context_get_state(context) != PA_CONTEXT_READY) {
    pa_mainloop_iterate(loop, 0, nullptr);
    std::this_thread::sleep_for(kIterateInterval);
  }

  if (pa_context_get_state(context) == PA_CONTEXT_READY) {
    auto nextScan = std::chrono::steady_clock::now();
    while (!m_stop.load(std::memory_order_acquire) &&
           pa_context_get_state(context) == PA_CONTEXT_READY) {
      pa_mainloop_iterate(loop, 0, nullptr);

      const auto now = std::chrono::steady_clock::now();
      if (now >= nextScan) {
        ScanState scan{this, false};
        pa_operation *op = pa_context_get_sink_input_info_list(
            context, sinkInputCallback, &scan);
        if (op) {
          pa_operation_unref(op);
        }
        nextScan = now + kScanInterval;
      }
      std::this_thread::sleep_for(kIterateInterval);
    }
  }

  setActive(false);
  pa_context_disconnect(context);
  pa_context_unref(context);
  pa_mainloop_free(loop);
}

} // namespace WallpiperKde
