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

// UsbVendorInterface for the tests, in place of UsbVendorInterfaceLibusb.cpp:
// every FakeLinUsbDevice plugged in is a device exposing the lin_usb interface.
// Status mapping as libusb's: STALL is LIBUSB_ERROR_PIPE, i.e. Status::Error.

#include "driver/UsbVendorInterface/UsbVendorInterface.h"

#include <chrono>

#include "FakeLinUsbDevice.h"

namespace
{

// The libusb handle slot holds the fake device.
FakeLinUsbDevice *device(libusb_device_handle *handle)
{
    return reinterpret_cast<FakeLinUsbDevice *>(handle);
}

UsbVendorInterface::Status toStatus(FakeLinUsbDevice::Result r)
{
    switch (r)
    {
    case FakeLinUsbDevice::Result::Ok:       return UsbVendorInterface::Status::Ok;
    case FakeLinUsbDevice::Result::Timeout:  return UsbVendorInterface::Status::Timeout;
    case FakeLinUsbDevice::Result::NoDevice: return UsbVendorInterface::Status::NoDevice;
    case FakeLinUsbDevice::Result::Stall:    break;
    }
    return UsbVendorInterface::Status::Error;
}

std::string text(FakeLinUsbDevice::Result r)
{
    switch (r)
    {
    case FakeLinUsbDevice::Result::Ok:       return "Success";
    case FakeLinUsbDevice::Result::Timeout:  return "Operation timed out";
    case FakeLinUsbDevice::Result::NoDevice: return "No such device (it may have been disconnected)";
    case FakeLinUsbDevice::Result::Stall:    break;
    }
    return "Pipe error";
}

} // namespace

UsbVendorInterface::~UsbVendorInterface()
{
    close();
}

int UsbVendorInterface::count(const Id &)
{
    return static_cast<int>(FakeLinUsbDevice::present().size());
}

bool UsbVendorInterface::open(const Id &, int index)
{
    close();
    const QList<FakeLinUsbDevice *> devices = FakeLinUsbDevice::present();
    if (index < 0 || index >= devices.size())
    {
        fail(Status::NoDevice, "open", "no device with this interface at index " + std::to_string(index));
        return false;
    }
    FakeLinUsbDevice *dev = devices.at(index);
    if (!dev->claim())
    {
        fail(Status::Error, "libusb_claim_interface", "Resource busy");
        return false;
    }
    _handle = reinterpret_cast<libusb_device_handle *>(dev);
    _itf = FakeLinUsbDevice::k_interface;
    _epIn = 0x83;
    _epOut = 0x04;
    return true;
}

void UsbVendorInterface::close()
{
    if (_handle)
    {
        device(_handle)->release();
        _handle = nullptr;
    }
    _itf = 0;
    _epIn = 0;
    _epOut = 0;
}

bool UsbVendorInterface::isOpen() const
{
    return _handle != nullptr;
}

UsbVendorInterface::Status UsbVendorInterface::bulkRead(void *data, int len, int &transferred, unsigned timeout_ms)
{
    transferred = 0;
    if (!_handle || !_epIn)
        return fail(Status::Error, "bulk IN", "device not open");
    const auto r = device(_handle)->bulkIn(data, len, transferred, std::chrono::milliseconds(timeout_ms));
    if (r == FakeLinUsbDevice::Result::Timeout)
        return Status::Timeout;
    return r == FakeLinUsbDevice::Result::Ok ? Status::Ok : fail(toStatus(r), "bulk IN", text(r));
}

UsbVendorInterface::Status UsbVendorInterface::bulkWrite(const void *data, int len, unsigned timeout_ms)
{
    if (!_handle || !_epOut)
        return fail(Status::Error, "bulk OUT", "device not open");
    const auto r = device(_handle)->bulkOut(data, len, std::chrono::milliseconds(timeout_ms));
    return r == FakeLinUsbDevice::Result::Ok ? Status::Ok : fail(toStatus(r), "bulk OUT", text(r));
}

UsbVendorInterface::Status UsbVendorInterface::controlIn(uint8_t bRequest, uint16_t wValue,
                                                         void *data, uint16_t len, unsigned)
{
    if (!_handle)
        return fail(Status::Error, "control IN", "device not open");
    const auto r = device(_handle)->control(true, bRequest, wValue, _itf, data, len);
    return r == FakeLinUsbDevice::Result::Ok ? Status::Ok : fail(toStatus(r), "control IN", text(r));
}

UsbVendorInterface::Status UsbVendorInterface::controlOut(uint8_t bRequest, uint16_t wValue,
                                                          const void *data, uint16_t len, unsigned)
{
    if (!_handle)
        return fail(Status::Error, "control OUT", "device not open");
    const auto r = device(_handle)->control(false, bRequest, wValue, _itf, const_cast<void *>(data), len);
    return r == FakeLinUsbDevice::Result::Ok ? Status::Ok : fail(toStatus(r), "control OUT", text(r));
}
