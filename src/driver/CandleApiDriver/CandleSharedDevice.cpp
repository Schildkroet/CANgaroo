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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>

#include <QDeadlineTimer>
#include <QMutexLocker>

#include "core/Log.h"

#include "api/candle_defs.h"

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
    lost.store(false, std::memory_order_relaxed);
    readerThread = std::thread([this]()
    {
        // A failed transfer (as opposed to a timeout) returns at once; without
        // a pause the loop would spin. Failing for about a second means the
        // device is gone.
        constexpr int k_maxFailures = 20;
        int failures = 0;
        while (readerRunning.load(std::memory_order_relaxed)) {
            candle_fd_frame_t frame;
            // Short timeout so the loop checks readerRunning frequently.
            if (!candle_fd_frame_read(handle, &frame, 50)) {
                if (candle_dev_last_error(handle) != CANDLE_ERR_READ_TIMEOUT) {
                    if (++failures >= k_maxFailures) {
                        lost.store(true, std::memory_order_relaxed);
                        QMutexLocker lock(&queueMutex);
                        queueCond.wakeAll();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            failures = 0;
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

// ── Discovery ────────────────────────────────────────────────────────────────

// Composite USB devices expose each CAN interface as a separate Windows device
// path containing "&mi_XX" (e.g. "&mi_00", "&mi_02"). Strip that segment so
// we can recognise multiple interface paths as the same physical device.
// Windows returns device paths in lowercase, so compare case-insensitively.
static std::wstring baseDevicePath(const std::wstring &path)
{
    // Normalise to lowercase so the map key is consistent regardless of capitalisation.
    std::wstring result = path;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);

    // Pass 1: strip every &mi_NNNN segment.
    // On serial-number-based composite USB devices &mi_XX appears in BOTH the
    // hardware-ID part AND the instance-ID part of the path, so a single erase
    // is not enough.  Loop until all occurrences are gone.
    const std::wstring mi_tag = L"&mi_";
    auto pos = result.find(mi_tag);
    while (pos != std::wstring::npos) {
        auto end = pos + mi_tag.size();
        while (end < result.size() && iswxdigit(result[end]))
            ++end;
        result.erase(pos, end - pos);
        pos = result.find(mi_tag, pos);
    }

    // Pass 2: for location-based instance IDs (no USB serial number) the
    // interface number is the last segment before #{GUID}, encoded as exactly
    // 4 hex digits (e.g. &0000 for MI_00, &0002 for MI_02).  Strip it so all
    // interfaces of the same physical device share the same key.
    const auto guid_pos = result.rfind(L"#{");
    if (guid_pos >= 5 && result[guid_pos - 5] == L'&') {
        bool all_hex = true;
        for (std::size_t k = guid_pos - 4; k < guid_pos; ++k) {
            if (!iswxdigit(result[k])) { all_hex = false; break; }
        }
        if (all_hex)
            result.erase(guid_pos - 5, 5);
    }

    return result;
}


// Map known VID/PID combinations to human-readable product names.
// The Windows device path is lower-case and contains "vid_XXXX&pid_XXXX".
static QString productNameFromPath(const std::wstring &path)
{
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    auto extract = [&](const wchar_t *tag) -> uint16_t {
        auto pos = lower.find(tag);
        if (pos == std::wstring::npos)
            return 0;
        return static_cast<uint16_t>(wcstoul(lower.c_str() + pos + wcslen(tag), nullptr, 16));
    };

    const uint16_t vid = extract(L"vid_");
    const uint16_t pid = extract(L"pid_");

    if (vid == 0x1209 && pid == 0xCA01) return QStringLiteral("CANnectivity");

    return QStringLiteral("candle");
}

std::vector<CandleDiscovered> candleDiscoverDevices()
{
    std::vector<CandleDiscovered> found;

    candle_list_handle clist;
    if (!candle_list_scan(&clist)) {
        return found;
    }

    uint8_t num_devices = 0;
    if (!candle_list_length(clist, &num_devices)) {
        candle_list_free(clist);
        return found;
    }

    int deviceCounter = 0;
    for (uint8_t i = 0; i < num_devices; i++) {
        candle_handle dev;
        if (!candle_dev_get(clist, i, &dev)) {
            log_error(QStringLiteral("CandleAPI: candle_dev_get failed for index %1").arg(static_cast<int>(i)));
            continue;
        }

        // Open temporarily to read channel count and per-channel capabilities.
        if (!candle_dev_open(dev)) {
            log_error(QStringLiteral("CandleAPI: discovery open failed for index %1").arg(static_cast<int>(i)));
            candle_dev_free(dev);
            continue;
        }

        uint8_t num_channels = 0;
        if (!candle_channel_count(dev, &num_channels) || num_channels == 0) {
            candle_dev_close(dev);
            candle_dev_free(dev);
            continue;
        }

        const std::wstring devPath(candle_dev_get_path(dev));
        const std::wstring baseKey = baseDevicePath(devPath);

        // Skip additional USB interfaces of a device we already processed.
        // On composite devices each CAN channel has its own interface path
        // (MI_00, MI_02, …) but they all share the same USB endpoint and
        // the firmware reports the total icount from any interface.
        const bool known = std::any_of(found.cbegin(), found.cend(),
                                       [&](const CandleDiscovered &d) { return d.baseKey == baseKey; });
        if (known) {
            candle_dev_close(dev);
            candle_dev_free(dev);
            continue;
        }

        // Create a single shared handle for all channels on this device.
        // candle_dev_get() allocates a fresh candle_device_t; we pre-populate
        // its capability fields so getAvailableBitrates() works before open().
        candle_handle shared_handle;
        candle_dev_get(clist, i, &shared_handle);
        {
            candle_device_t *src = static_cast<candle_device_t*>(dev);
            candle_device_t *dst = static_cast<candle_device_t*>(shared_handle);
            wcscpy(dst->path, src->path);
            dst->state   = src->state;
            dst->dconf   = src->dconf;
            dst->bt_const = src->bt_const;
            memcpy(dst->ch_caps, src->ch_caps, sizeof(src->ch_caps));
        }

        candle_dev_close(dev);
        candle_dev_free(dev);

        auto sharedDev = std::make_shared<CandleSharedDevice>();
        sharedDev->handle = shared_handle;
        sharedDev->productName = productNameFromPath(devPath);
        sharedDev->deviceIndex = deviceCounter++;
        found.push_back(CandleDiscovered{.device = sharedDev, .channels = num_channels, .baseKey = baseKey});
    }

    candle_list_free(clist);
    return found;
}
