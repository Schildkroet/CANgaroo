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

#include "UsbVendorInterface.h"

#include <libusb.h>

namespace
{
constexpr uint8_t BRT_VENDOR_ITF_OUT = 0x41u; // vendor | interface | host->device
constexpr uint8_t BRT_VENDOR_ITF_IN  = 0xC1u; // vendor | interface | device->host

// True if dev matches id; fills the interface number and bulk endpoints.
bool findInterface(libusb_device *dev, const UsbVendorInterface::Id &id,
                   uint8_t &itf, uint8_t &epIn, uint8_t &epOut)
{
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != 0)
        return false;
    if (desc.idVendor != id.vid || desc.idProduct != id.pid)
        return false;

    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
        return false;

    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; i++)
    {
        const libusb_interface &ifc = cfg->interface[i];
        for (int a = 0; a < ifc.num_altsetting && !found; a++)
        {
            const libusb_interface_descriptor &alt = ifc.altsetting[a];
            if (alt.bInterfaceClass    != LIBUSB_CLASS_VENDOR_SPEC ||
                alt.bInterfaceSubClass != 0xFFu ||
                alt.bInterfaceProtocol != id.protocol)
            {
                continue;
            }
            itf = alt.bInterfaceNumber;
            for (uint8_t e = 0; e < alt.bNumEndpoints; e++)
            {
                const libusb_endpoint_descriptor &ep = alt.endpoint[e];
                if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                    epIn = ep.bEndpointAddress;
                else
                    epOut = ep.bEndpointAddress;
            }
            found = true;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

UsbVendorInterface::Status toStatus(int rc)
{
    switch (rc)
    {
    case LIBUSB_SUCCESS:          return UsbVendorInterface::Status::Ok;
    case LIBUSB_ERROR_TIMEOUT:    return UsbVendorInterface::Status::Timeout;
    case LIBUSB_ERROR_NO_DEVICE:  return UsbVendorInterface::Status::NoDevice;
    default:                      return UsbVendorInterface::Status::Error;
    }
}

std::string errorText(int rc)
{
    return libusb_strerror(static_cast<libusb_error>(rc));
}
} // namespace

UsbVendorInterface::~UsbVendorInterface()
{
    close();
}

int UsbVendorInterface::count(const Id &id)
{
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return 0;

    libusb_device **list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    int matches = 0;
    for (ssize_t i = 0; i < cnt; i++)
    {
        uint8_t itf = 0, epIn = 0, epOut = 0;
        if (findInterface(list[i], id, itf, epIn, epOut))
            matches++;
    }
    if (cnt >= 0)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return matches;
}

bool UsbVendorInterface::open(const Id &id, int index)
{
    close();

    // Own context per handle so open/close of one device never affects another.
    if (libusb_init(&_ctx) != 0)
    {
        _ctx = nullptr;
        fail(Status::Error, "libusb_init", "failed");
        return false;
    }

    libusb_device **list = nullptr;
    const ssize_t cnt = libusb_get_device_list(_ctx, &list);
    if (cnt < 0)
    {
        fail(Status::Error, "libusb_get_device_list", errorText(static_cast<int>(cnt)));
        close();
        return false;
    }

    // The VID/PID may be shared with devices lacking this interface, so only
    // matching interfaces count towards index.
    libusb_device *found = nullptr;
    int matchIndex = 0;
    for (ssize_t i = 0; i < cnt && !found; i++)
    {
        if (findInterface(list[i], id, _itf, _epIn, _epOut) && matchIndex++ == index)
            found = list[i];
    }

    int rc = found ? libusb_open(found, &_handle) : LIBUSB_ERROR_NOT_FOUND;
    libusb_free_device_list(list, 1);
    if (!found)
    {
        fail(Status::NoDevice, "open", "no device with this interface at index " + std::to_string(index));
        close();
        return false;
    }
    if (rc != 0)
    {
        _handle = nullptr;
        if (rc == LIBUSB_ERROR_ACCESS)
            fail(Status::Error, "open", "permission denied. Add a udev rule (see README) or run as root.");
        else
            fail(toStatus(rc), "libusb_open", errorText(rc));
        close();
        return false;
    }

    if (libusb_kernel_driver_active(_handle, _itf) == 1)
    {
        rc = libusb_detach_kernel_driver(_handle, _itf);
        if (rc != 0)
        {
            fail(toStatus(rc), "libusb_detach_kernel_driver", errorText(rc));
            close();
            return false;
        }
        _kernelDriverDetached = true;
    }

    rc = libusb_claim_interface(_handle, _itf);
    if (rc != 0)
    {
        fail(toStatus(rc), "libusb_claim_interface", errorText(rc));
        close();
        return false;
    }
    return true;
}

void UsbVendorInterface::close()
{
    if (_handle)
    {
        libusb_release_interface(_handle, _itf);
        if (_kernelDriverDetached)
            libusb_attach_kernel_driver(_handle, _itf);
        libusb_close(_handle);
        _handle = nullptr;
    }
    if (_ctx)
    {
        libusb_exit(_ctx);
        _ctx = nullptr;
    }
    _kernelDriverDetached = false;
    _itf   = 0;
    _epIn  = 0;
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
    const int rc = libusb_bulk_transfer(_handle, _epIn, static_cast<unsigned char *>(data),
                                        len, &transferred, timeout_ms);
    // Timeouts are routine for the polling reader: not an error to report.
    if (rc == LIBUSB_ERROR_TIMEOUT)
        return Status::Timeout;
    return rc == 0 ? Status::Ok : fail(toStatus(rc), "bulk IN", errorText(rc));
}

UsbVendorInterface::Status UsbVendorInterface::bulkWrite(const void *data, int len, unsigned timeout_ms)
{
    if (!_handle || !_epOut)
        return fail(Status::Error, "bulk OUT", "device not open");
    int transferred = 0;
    // libusb takes a non-const buffer but does not modify it for OUT transfers.
    auto *buf = static_cast<unsigned char *>(const_cast<void *>(data));
    const int rc = libusb_bulk_transfer(_handle, _epOut, buf, len, &transferred, timeout_ms);
    if (rc != 0)
        return fail(toStatus(rc), "bulk OUT", errorText(rc));
    if (transferred != len)
        return fail(Status::Error, "bulk OUT", "short write");
    return Status::Ok;
}

UsbVendorInterface::Status UsbVendorInterface::controlIn(uint8_t bRequest, uint16_t wValue,
                                                         void *data, uint16_t len, unsigned timeout_ms)
{
    if (!_handle)
        return fail(Status::Error, "control IN", "device not open");
    const int rc = libusb_control_transfer(_handle, BRT_VENDOR_ITF_IN, bRequest, wValue, _itf,
                                           static_cast<unsigned char *>(data), len, timeout_ms);
    return rc < 0 ? fail(toStatus(rc), "control IN", errorText(rc)) : Status::Ok;
}

UsbVendorInterface::Status UsbVendorInterface::controlOut(uint8_t bRequest, uint16_t wValue,
                                                          const void *data, uint16_t len, unsigned timeout_ms)
{
    if (!_handle)
        return fail(Status::Error, "control OUT", "device not open");
    auto *buf = static_cast<unsigned char *>(const_cast<void *>(data));
    const int rc = libusb_control_transfer(_handle, BRT_VENDOR_ITF_OUT, bRequest, wValue, _itf,
                                           buf, len, timeout_ms);
    return rc < 0 ? fail(toStatus(rc), "control OUT", errorText(rc)) : Status::Ok;
}
