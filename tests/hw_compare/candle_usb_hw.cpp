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

// candle_usb.h for slcan_hw_compare: a real gs_usb adapter through libusb, and
// FakeGsUsbDevice for paths starting with "fake-", so both can be run side by
// side. Only the adapter whose USB serial number is in CANDLE_SERIAL is
// touched: other gs_usb devices (e.g. the bus partner) keep their kernel driver.

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include <libusb.h>

#include "driver/CandleApiDriver/api/candle_usb.h"

#include "candle_driver/FakeGsUsbDevice.h"

struct candle_usb
{
    FakeGsUsbDevice *fake{nullptr};
    libusb_device_handle *handle{nullptr};
    uint8_t epIn{0};
    uint8_t epOut{0};
    bool reading{false};
};

namespace
{

libusb_context *context()
{
    static libusb_context *ctx = [] {
        libusb_context *c = nullptr;
        libusb_init(&c);
        return c;
    }();
    return ctx;
}

std::string wantedSerial()
{
    const char *s = std::getenv("CANDLE_SERIAL");
    return s ? s : "";
}

// gs_usb candleLight / CANable Candlelight
bool isGsUsb(const libusb_device_descriptor &d)
{
    return d.idVendor == 0x1d50 && d.idProduct == 0x606f;
}

std::string serialOf(libusb_device *dev, const libusb_device_descriptor &d)
{
    libusb_device_handle *h = nullptr;
    if (d.iSerialNumber == 0 || libusb_open(dev, &h) != 0)
        return {};
    unsigned char buf[128] = {};
    const int n = libusb_get_string_descriptor_ascii(h, d.iSerialNumber, buf, sizeof(buf));
    libusb_close(h);
    return n > 0 ? std::string(reinterpret_cast<char *>(buf), static_cast<size_t>(n)) : std::string();
}

std::wstring pathOf(libusb_device *dev)
{
    return L"usb:" + std::to_wstring(libusb_get_bus_number(dev)) + L":" + std::to_wstring(libusb_get_device_address(dev));
}

} // namespace

extern "C" {

int candle_usb_scan(wchar_t paths[][CANDLE_USB_PATH_LEN], int max, candle_err_t *err)
{
    int n = 0;
    for (const FakeGsUsbDevice *d : FakeGsUsbDevice::present())
    {
        if (n < max)
            std::wcsncpy(paths[n++], d->path().c_str(), CANDLE_USB_PATH_LEN - 1);
    }

    const std::string serial = wantedSerial();
    libusb_device **list = nullptr;
    const ssize_t count = serial.empty() ? 0 : libusb_get_device_list(context(), &list);
    for (ssize_t i = 0; i < count && n < max; ++i)
    {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0 || !isGsUsb(d))
            continue;
        if (serialOf(list[i], d) != serial)
            continue;
        std::wcsncpy(paths[n++], pathOf(list[i]).c_str(), CANDLE_USB_PATH_LEN - 1);
    }
    if (list)
        libusb_free_device_list(list, 1);
    *err = CANDLE_ERR_OK;
    return n;
}

candle_usb_t *candle_usb_open(const wchar_t *path, bool start_reads, candle_err_t *err)
{
    *err = CANDLE_ERR_CREATE_FILE;
    if (std::wcsncmp(path, L"fake-", 5) == 0)
    {
        FakeGsUsbDevice *dev = FakeGsUsbDevice::byPath(path);
        if (!dev || !dev->usbOpen())
            return nullptr;
        *err = CANDLE_ERR_OK;
        auto *usb = new candle_usb;
        usb->fake = dev;
        usb->reading = start_reads;
        return usb;
    }

    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(context(), &list);
    libusb_device_handle *h = nullptr;
    uint8_t epIn = 0, epOut = 0;
    for (ssize_t i = 0; i < count && !h; ++i)
    {
        if (pathOf(list[i]) != path)
            continue;
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(list[i], &cfg) == 0)
        {
            const libusb_interface_descriptor &alt = cfg->interface[0].altsetting[0];
            for (int e = 0; e < alt.bNumEndpoints; ++e)
            {
                const libusb_endpoint_descriptor &ep = alt.endpoint[e];
                if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (ep.bEndpointAddress & 0x80)
                    epIn = epIn ? epIn : ep.bEndpointAddress;
                else
                    epOut = epOut ? epOut : ep.bEndpointAddress;
            }
            libusb_free_config_descriptor(cfg);
        }
        if (libusb_open(list[i], &h) != 0)
            h = nullptr;
    }
    if (list)
        libusb_free_device_list(list, 1);
    if (!h)
        return nullptr;

    // Take interface 0 from the kernel's gs_usb driver for as long as it is open
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0 || !epIn || !epOut)
    {
        libusb_close(h);
        *err = CANDLE_ERR_WINUSB_INITIALIZE;
        return nullptr;
    }
    *err = CANDLE_ERR_OK;
    auto *usb = new candle_usb;
    usb->handle = h;
    usb->epIn = epIn;
    usb->epOut = epOut;
    usb->reading = start_reads;
    return usb;
}

void candle_usb_close(candle_usb_t *usb)
{
    if (!usb)
        return;
    if (usb->fake)
        usb->fake->usbClose();
    if (usb->handle)
    {
        libusb_release_interface(usb->handle, 0);  // auto detach: gs_usb gets it back
        libusb_close(usb->handle);
    }
    delete usb;
}

uint8_t candle_usb_interface_number(const candle_usb_t *)
{
    return 0;
}

bool candle_usb_control(candle_usb_t *usb, uint8_t request, uint8_t requesttype,
                        uint16_t value, uint16_t index, void *data, uint16_t size)
{
    if (usb->fake)
        return usb->fake->control(request, requesttype, value, index, static_cast<uint8_t *>(data), size);
    return libusb_control_transfer(usb->handle, requesttype, request, value, index,
                                   static_cast<unsigned char *>(data), size, 1000) >= 0;
}

bool candle_usb_write(candle_usb_t *usb, const void *data, uint32_t len, uint32_t timeout_ms)
{
    if (usb->fake)
        return usb->fake->bulkOut(static_cast<const uint8_t *>(data), len);
    int sent = 0;
    return libusb_bulk_transfer(usb->handle, usb->epOut, static_cast<unsigned char *>(const_cast<void *>(data)),
                                static_cast<int>(len), &sent, timeout_ms) == 0 && sent == static_cast<int>(len);
}

candle_err_t candle_usb_read(candle_usb_t *usb, uint8_t *buf, uint32_t bufsize,
                             uint32_t *len, uint32_t timeout_ms)
{
    if (!usb->reading)
        return CANDLE_ERR_READ_WAIT;
    if (usb->fake)
    {
        const int rc = usb->fake->bulkIn(buf, bufsize, len, timeout_ms);
        return rc == 0 ? CANDLE_ERR_OK : rc == 1 ? CANDLE_ERR_READ_TIMEOUT : CANDLE_ERR_READ_RESULT;
    }
    int got = 0;
    const int rc = libusb_bulk_transfer(usb->handle, usb->epIn, buf, static_cast<int>(bufsize), &got, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT && got == 0)
        return CANDLE_ERR_READ_TIMEOUT;
    if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT)
        return CANDLE_ERR_READ_RESULT;
    *len = static_cast<uint32_t>(got);
    return CANDLE_ERR_OK;
}

} // extern "C"
