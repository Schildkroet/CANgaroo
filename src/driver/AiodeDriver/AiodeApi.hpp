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

#pragma once

#include "driver/GpioProvider.h"
#include "aio_usb_protocol.h"

/* CMake/qmake provides the libusb include directory so <libusb.h> resolves on
 * both Linux (/usr/include/libusb-1.0) and Windows (vcpkg / manual install). */
#include <libusb.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <QList>
#include <QMutex>
#include <QString>

// Host driver for the aio_usb USB GPIO/analog interface (bInterfaceProtocol
// 0x02) on the Candlelight-family composite device. Implements GpioProvider so
// the GPIO Control window can drive it exactly like a GrIP device.
//
// State reports are obtained by polling AIO_USB_BREQ_READ_STATUS from a
// background thread; each report is delivered via GpioProvider::gpioUpdated().
class AiodeApi : public GpioProvider
{
    Q_OBJECT

public:
    explicit AiodeApi(QObject *parent = nullptr);
    ~AiodeApi() override;

    AiodeApi(const AiodeApi &) = delete;
    AiodeApi &operator=(const AiodeApi &) = delete;

    // Discover and open every connected aiode device. Each returned instance is
    // already open and parented to @p parent. Returns an empty list if none are
    // present. Caller owns the instances (delete to close).
    static QList<AiodeApi *> scan(QObject *parent);

    bool isOpen() const { return dev_ != nullptr; }

    // ---- GpioProvider ----
    QString name() const override { return name_; }
    int  digitalPinCount() const override;
    int  analogPinCount() const override;
    void setConfig(bool enable, uint8_t cycleMs, uint16_t dirMask) override;
    void setOutput(uint16_t outputMask) override;

    const std::string &lastError() const { return last_error_; }

private:
    // Open the device at the given USB bus/address and claim its AIO interface.
    bool openByAddress(uint8_t bus, uint8_t addr, int index);
    // Open an already-referenced device handle and claim its AIO interface.
    bool openDevice(libusb_device *device, int index);
    void close();

    bool controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool controlIn (uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool setError(int libusb_rc, const char *context);

    bool setHostFormat();
    bool getCaps(aio_usb_caps_t &caps);
    bool readStatus(aio_usb_report_t &report);

    void startPolling();
    void stopPolling();
    void pollLoop();

    libusb_context       *ctx_  = nullptr;
    libusb_device_handle *dev_  = nullptr;
    uint8_t  ep_in_  = 0;
    uint8_t  ep_out_ = 0;
    uint8_t  itf_    = 0;
    bool     kernel_driver_detached_ = false;

    aio_usb_caps_t caps_{};
    QString        name_{"aiode"};
    std::string    last_error_;

    // Serialises control transfers between the poll thread and the GUI thread
    // (setConfig / setOutput).
    QMutex usbMutex_;

    std::thread        pollThread_;
    std::atomic<bool>  pollRunning_{false};
    std::atomic<uint16_t> dirMask_{0};   ///< bit set = output line
    std::atomic<uint16_t> outMask_{0};   ///< last requested output levels
    std::atomic<uint32_t> cycleMs_{50};
};
