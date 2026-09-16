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

#include "CandleSharedDevice.h"

#include "core/Log.h"

#include <QMutexLocker>
#include <QDeadlineTimer>

CandleSharedDevice::~CandleSharedDevice()
{
    stopReader();
    if (handle) {
        if (openCount > 0) {
            candle_dev_close(handle);
        }
        candle_dev_free(handle);
    }
}

void CandleSharedDevice::startReader()
{
    readerRunning.store(true, std::memory_order_relaxed);
    readerThread = std::thread([this]()
    {
        while (readerRunning.load(std::memory_order_relaxed)) {
            candle_fd_frame_t frame;
            // Short timeout so the loop checks readerRunning frequently.
            if (!candle_fd_frame_read(handle, &frame, 50)) {
                continue;
            }
            const candle_frametype_t frameType = candle_fd_frame_type(&frame);
            // Echoes are TX confirmations: CandleApiInterface only shows a sent
            // frame once the device has echoed it back.
            if (frameType != CANDLE_FRAMETYPE_RECEIVE
                    && frameType != CANDLE_FRAMETYPE_ERROR
                    && frameType != CANDLE_FRAMETYPE_ECHO) {
                continue;
            }
            const uint8_t ch = frame.channel;
            if (ch < MAX_CHANNELS) {
                CandleQueuedFrame queuedFrame;
                queuedFrame.frame = frame;
                queuedFrame.timestampValid = deviceTimestampToHostUs(candle_fd_frame_timestamp_us(&frame),
                                                                     queuedFrame.timestampUs);

                QMutexLocker lock(&queueMutex);
                rxQueues[ch].append(queuedFrame);
                queueCond.wakeAll();
            } else {
                log_error(QStringLiteral("CandleAPI: received frame for invalid channel %1").arg(static_cast<int>(ch)));
            }
        }
    });
}

void CandleSharedDevice::stopReader()
{
    readerRunning.store(false, std::memory_order_relaxed);
    if (readerThread.joinable()) {
        readerThread.join();
    }
    QMutexLocker lock(&queueMutex);
    for (auto &q : rxQueues) {
        q.clear();
    }
}

void CandleSharedDevice::resetTimestampEpoch(uint64_t hostOffsetUs, uint32_t deviceTicks, bool valid)
{
    QMutexLocker lock(&timestampMutex);
    hostOffsetStart = hostOffsetUs;
    deviceTicksStart = deviceTicks;
    deviceTimestampValid = valid;
    prevDeviceTs = 0;
    deviceTsHigh = 0;
}

bool CandleSharedDevice::deviceTimestampToHostUs(uint32_t rawTimestampUs, uint64_t &hostTimestampUs)
{
    constexpr uint32_t wrapThreshold = 0x80000000u;

    QMutexLocker lock(&timestampMutex);

    if (!deviceTimestampValid) {
        return false;
    }

    if (prevDeviceTs != 0
            && rawTimestampUs < prevDeviceTs
            && (prevDeviceTs - rawTimestampUs) > wrapThreshold) {
        deviceTsHigh += (1ULL << 32);
    }

    uint64_t absTimestampUs = static_cast<uint64_t>(rawTimestampUs) + deviceTsHigh;

    // If the first timestamp observed after opening is below the epoch sample,
    // distinguish a real wrap from a small pre-epoch buffered frame.
    if (prevDeviceTs == 0 && absTimestampUs < deviceTicksStart) {
        if ((deviceTicksStart - absTimestampUs) > wrapThreshold) {
            deviceTsHigh += (1ULL << 32);
            absTimestampUs += (1ULL << 32);
        } else {
            return false;
        }
    }

    if (absTimestampUs < deviceTicksStart) {
        return false;
    }

    prevDeviceTs = rawTimestampUs;
    hostTimestampUs = hostOffsetStart + (absTimestampUs - deviceTicksStart);
    return true;
}

bool CandleSharedDevice::readFrame(uint8_t channel, CandleQueuedFrame &queuedFrame, unsigned timeout_ms)
{
    QMutexLocker lock(&queueMutex);
    const QDeadlineTimer deadline(timeout_ms);
    while (rxQueues[channel].isEmpty()) {
        if (!queueCond.wait(&queueMutex, deadline)) {
            return false;
        }
    }
    queuedFrame = rxQueues[channel].takeFirst();
    return true;
}
