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

#include <atomic>
#include <thread>

namespace WallpiperKde {

/*
 * Detects any uncorked, unmuted PulseAudio/PipeWire sink input except Wallpaper
 * Engine's own stream.  The result is published to the COFF shim so WE's
 * `playbackaudio` setting can be used for native Linux audio too.
 */
class AudioActivityWatcher {
public:
  AudioActivityWatcher();
  ~AudioActivityWatcher();

  AudioActivityWatcher(const AudioActivityWatcher &) = delete;
  AudioActivityWatcher &operator=(const AudioActivityWatcher &) = delete;

  bool active() const { return m_active.load(std::memory_order_acquire); }

  void setActive(bool active) {
    m_active.store(active, std::memory_order_release);
  }

private:
  void run();

  std::atomic<bool> m_active{false};
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
};

} // namespace WallpiperKde
