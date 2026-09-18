/*

  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

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

#include "AiodeApi.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <QVector>

static constexpr unsigned CTRL_TIMEOUT_MS = 1000u;

/* -------------------------------------------------------------------------
 * Construction / destruction
 * ------------------------------------------------------------------------- */

AiodeApi::AiodeApi(QObject *parent)
    : GpioProvider(parent)
{
    // gpioUpdated() is emitted from the poll thread and delivered via a queued
    // connection, which copies the arguments — register the container type.
    qRegisterMetaType<QVector<uint16_t>>("QVector<uint16_t>");
}

AiodeApi::~AiodeApi()
{
    close();
}

/* -------------------------------------------------------------------------
 * Discovery
 * ------------------------------------------------------------------------- */

QList<AiodeApi *> AiodeApi::scan(QObject *parent)
{
    QList<AiodeApi *> result;
    const int count = UsbVendorInterface::count(USB_ID);
    for (int i = 0; i < count; ++i)
    {
        auto *api = new AiodeApi(parent);
        if (api->open(i))
            result.append(api);
        else
            delete api;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------- */

bool AiodeApi::open(int index)
{
    if (!usb_.open(USB_ID, index))
    {
        last_error_ = usb_.lastError();
        return false;
    }

    setHostFormat();

    if (!getCaps(caps_))
    {
        close();
        return false;
    }

    name_ = QStringLiteral("Aiode Device %1").arg(index + 1);
    last_error_.clear();
    return true;
}

void AiodeApi::close()
{
    stopPolling();
    usb_.close();
}

/* -------------------------------------------------------------------------
 * Low-level control transfers
 * ------------------------------------------------------------------------- */

bool AiodeApi::controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    if (usb_.controlOut(breq, wValue, data, len, CTRL_TIMEOUT_MS) != UsbVendorInterface::Status::Ok)
    {
        last_error_ = usb_.lastError();
        return false;
    }
    return true;
}

bool AiodeApi::controlIn(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    if (usb_.controlIn(breq, wValue, data, len, CTRL_TIMEOUT_MS) != UsbVendorInterface::Status::Ok)
    {
        last_error_ = usb_.lastError();
        return false;
    }
    return true;
}

bool AiodeApi::setHostFormat()
{
    aio_usb_host_config_t cfg{};
    cfg.byte_order = 0x0000beefu; /* little-endian marker; firmware ignores value */
    return controlOut(AIO_USB_BREQ_HOST_FORMAT, 0, &cfg, sizeof(cfg));
}

bool AiodeApi::getCaps(aio_usb_caps_t &caps)
{
    std::memset(&caps, 0, sizeof(caps));
    return controlIn(AIO_USB_BREQ_DEVICE_CONFIG, 0, &caps, sizeof(caps));
}

bool AiodeApi::readStatus(aio_usb_report_t &report)
{
    std::memset(&report, 0, sizeof(report));
    return controlIn(AIO_USB_BREQ_READ_STATUS, 0, &report, sizeof(report));
}

/* -------------------------------------------------------------------------
 * GpioProvider interface
 * ------------------------------------------------------------------------- */

int AiodeApi::digitalPinCount() const
{
    return std::min<int>(16, caps_.io_count);
}

int AiodeApi::analogPinCount() const
{
    if (!(caps_.features & AIO_USB_FEATURE_ANALOG))
        return 0;
    return std::min<int>(16, caps_.analog_count);
}

void AiodeApi::setConfig(bool enable, uint16_t cycleMs, uint16_t dirMask)
{
    // Stop the poll thread first so reconfiguration owns the USB exclusively.
    stopPolling();

    dirMask_ = dirMask;
    cycleMs_ = std::max<uint32_t>(5u, cycleMs);

    if (!enable)
        return;

    QMutexLocker lock(&usbMutex_);
    const int pins = digitalPinCount();
    for (int line = 0; line < pins; ++line)
    {
        aio_usb_io_config_t cfg{};
        cfg.mode          = (dirMask >> line) & 1u ? AIO_USB_IO_MODE_OUTPUT
                                                   : AIO_USB_IO_MODE_INPUT;
        cfg.default_state = 0;
        cfg.flags         = 0;
        cfg.auto_report_ms = 0; /* host polls READ_STATUS instead */
        controlOut(AIO_USB_BREQ_IO_CONFIG, static_cast<uint16_t>(line), &cfg, sizeof(cfg));
    }

    // Apply the current desired output levels for the freshly-configured outputs.
    aio_usb_io_set_t set{};
    set.mask   = dirMask;
    set.values = static_cast<uint32_t>(outMask_.load() & dirMask);
    controlOut(AIO_USB_BREQ_IO_SET, 0, &set, sizeof(set));
    lock.unlock();

    startPolling();
}

void AiodeApi::setOutput(uint16_t outputMask)
{
    outMask_ = outputMask;

    const uint16_t dir = dirMask_.load();
    aio_usb_io_set_t set{};
    set.mask   = dir;
    set.values = static_cast<uint32_t>(outputMask & dir);

    QMutexLocker lock(&usbMutex_);
    controlOut(AIO_USB_BREQ_IO_SET, 0, &set, sizeof(set));
}

/* -------------------------------------------------------------------------
 * Background polling
 * ------------------------------------------------------------------------- */

void AiodeApi::startPolling()
{
    if (pollRunning_.exchange(true))
        return; /* already running */
    pollThread_ = std::thread(&AiodeApi::pollLoop, this);
}

void AiodeApi::stopPolling()
{
    if (!pollRunning_.exchange(false))
        return;
    if (pollThread_.joinable())
        pollThread_.join();
}

void AiodeApi::pollLoop()
{
    while (pollRunning_.load())
    {
        aio_usb_report_t report{};
        bool ok = false;
        {
            QMutexLocker lock(&usbMutex_);
            ok = readStatus(report);
        }

        if (ok)
        {
            const int analogCount = analogPinCount();
            QVector<uint16_t> analog(analogCount);
            for (int i = 0; i < analogCount; ++i)
                analog[i] = report.analog[i];

            emit gpioUpdated(static_cast<uint16_t>(report.io_states & 0xFFFFu), analog);
        }

        // Sleep in small slices so a disable request stops us promptly.
        uint32_t remaining = cycleMs_.load();
        while (remaining > 0 && pollRunning_.load())
        {
            const uint32_t slice = std::min<uint32_t>(remaining, 20u);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
    }
}
