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

#include <atomic>
#include <cstdint>
#include <string>

#include <QMutex>

struct libusb_context;
struct libusb_device_handle;

// One vendor-specific interface (class/subclass 0xFF, identified by its
// bInterfaceProtocol) of a composite USB device, opened on its own.
//
// Linux/macOS use libusb. Windows uses WinUSB directly on the interface's
// device node, found by the DeviceInterfaceGUID the firmware sets in its
// MS OS 2.0 descriptor. libusb cannot be used there: libusb_open() opens every
// WinUSB interface of a composite device, and WinUSB allows one handle per
// interface, so it fails with "access denied" whenever another driver (e.g.
// CandleApiDriver on gs_usb interface 0) already holds a sibling interface.
//
// Transfers may be issued from several threads at once (a bulk reader plus
// control requests); callers serialise transfers on the same endpoint.
class UsbVendorInterface
{
public:
    enum class Status
    {
        Ok,
        Timeout,
        NoDevice,
        Error,
    };

    struct Id
    {
        uint16_t    vid;
        uint16_t    pid;
        uint8_t     protocol;     // bInterfaceProtocol
        const char *windowsGuid;  // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
    };

    UsbVendorInterface() = default;
    ~UsbVendorInterface();

    UsbVendorInterface(const UsbVendorInterface &) = delete;
    UsbVendorInterface &operator=(const UsbVendorInterface &) = delete;

    // Number of attached devices exposing the interface.
    [[nodiscard]] static int count(const Id &id);

    // Open the index-th device exposing the interface and claim it.
    [[nodiscard]] bool open(const Id &id, int index);
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] uint8_t interfaceNumber() const { return _itf; }

    Status bulkRead(void *data, int len, int &transferred, unsigned timeout_ms);
    Status bulkWrite(const void *data, int len, unsigned timeout_ms);

    // Vendor requests addressed to this interface (wIndex = interface number).
    Status controlIn(uint8_t bRequest, uint16_t wValue, void *data, uint16_t len, unsigned timeout_ms);
    Status controlOut(uint8_t bRequest, uint16_t wValue, const void *data, uint16_t len, unsigned timeout_ms);

    // Description of the most recent failure.
    [[nodiscard]] std::string lastError() const;

private:
    Status fail(Status status, const std::string &context, const std::string &reason);

    uint8_t _itf{0};
    uint8_t _epIn{0};
    uint8_t _epOut{0};

    mutable QMutex _errorMutex;
    std::string    _lastError;

#ifdef _WIN32
    void *_file{nullptr};    // HANDLE, INVALID_HANDLE_VALUE stored as nullptr
    void *_winusb{nullptr};  // WINUSB_INTERFACE_HANDLE
    // Last PIPE_TRANSFER_TIMEOUT set per pipe: [0] control, [1] bulk IN, [2] bulk OUT
    std::atomic<unsigned> _pipeTimeout[3]{};
    bool setPipeTimeout(int slot, uint8_t pipeId, unsigned timeout_ms);
#else
    libusb_context       *_ctx{nullptr};
    libusb_device_handle *_handle{nullptr};
    bool                  _kernelDriverDetached{false};
#endif
};
