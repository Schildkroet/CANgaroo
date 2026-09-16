/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "api/candle.h"

#include <QList>
#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>
#include <thread>

struct CandleQueuedFrame
{
    candle_fd_frame_t frame{};
    uint64_t timestampUs{0};
    bool timestampValid{false};
};

// Shared state for all CAN channels on a single physical gs_usb device.
//
// gs_usb devices multiplex all channels through one USB bulk IN endpoint.
// Opening the device multiple times creates competing overlapped reads on that
// endpoint, so frames for channel 1 are silently consumed by channel 0's reads
// and lost. This struct owns exactly ONE USB handle and ONE reader thread per
// physical device; all CandleApiInterface instances for the same device share
// it and read from their per-channel queue.
struct CandleSharedDevice
{
    static constexpr int MAX_CHANNELS = 8;

    candle_handle handle{nullptr};
    QString productName{"Candle"};
    int deviceIndex{0};

    // Open/close reference count — protected by openMutex.
    // The USB device is opened when the count reaches 1 and closed when it
    // drops back to 0.
    QMutex openMutex;
    int openCount{0};

    // Serialises candle_frame_send / candle_fd_frame_send calls. Both channels
    // share the same USB bulk OUT endpoint; without a mutex two concurrent sends
    // could interleave. The tryLock timeout in sendMessage() ensures that a
    // stuck write on one channel does not block the other channel indefinitely.
    QMutex writeMutex;

    // Per-channel receive queues fed by the background reader thread.
    QMutex queueMutex;
    QWaitCondition queueCond;
    QList<CandleQueuedFrame> rxQueues[MAX_CHANNELS];

    // Shared timestamp epoch captured once at device open time so that all
    // channels on the same physical device use an identical baseline.
    uint64_t hostOffsetStart{0};
    uint32_t deviceTicksStart{0};
    bool deviceTimestampValid{false};

    // 32-bit firmware timestamps wrap after about 71 minutes. Unwrap them in
    // the single USB reader/TX order so all channels share one monotonic clock.
    QMutex timestampMutex;
    uint32_t prevDeviceTs{0};
    uint64_t deviceTsHigh{0};

    // Background reader thread: calls candle_fd_frame_read() in a loop and
    // routes each received frame to the appropriate channel queue.
    std::thread readerThread;
    std::atomic<bool> readerRunning{false};

    ~CandleSharedDevice();

    void startReader();
    void stopReader();
    void resetTimestampEpoch(uint64_t hostOffsetUs, uint32_t deviceTicks, bool valid);
    bool deviceTimestampToHostUs(uint32_t rawTimestampUs, uint64_t &hostTimestampUs);

    // Block until a frame for channel arrives or timeout_ms elapses.
    bool readFrame(uint8_t channel, CandleQueuedFrame &queuedFrame, unsigned timeout_ms);
};
