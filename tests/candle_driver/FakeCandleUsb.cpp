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

// candle_usb.h for the tests: every FakeGsUsbDevice alive is a device present.

#include <cwchar>

#include "driver/CandleApiDriver/api/candle_usb.h"

#include "FakeGsUsbDevice.h"

struct candle_usb
{
    FakeGsUsbDevice *dev;
    bool reading;
};

extern "C" {

int candle_usb_scan(wchar_t paths[][CANDLE_USB_PATH_LEN], int max, candle_err_t *err)
{
    int n = 0;
    for (const FakeGsUsbDevice *d : FakeGsUsbDevice::present())
    {
        if (n >= max)
            break;
        std::wcsncpy(paths[n], d->path().c_str(), CANDLE_USB_PATH_LEN - 1);
        paths[n][CANDLE_USB_PATH_LEN - 1] = 0;
        n++;
    }
    *err = CANDLE_ERR_OK;
    return n;
}

candle_usb_t *candle_usb_open(const wchar_t *path, bool start_reads, candle_err_t *err)
{
    FakeGsUsbDevice *dev = FakeGsUsbDevice::byPath(path);
    if (dev == nullptr || !dev->usbOpen())
    {
        *err = CANDLE_ERR_CREATE_FILE;  // as CreateFile() fails on a device in use
        return nullptr;
    }
    *err = CANDLE_ERR_OK;
    return new candle_usb{dev, start_reads};
}

void candle_usb_close(candle_usb_t *usb)
{
    if (usb == nullptr)
        return;
    usb->dev->usbClose();
    delete usb;
}

uint8_t candle_usb_interface_number(const candle_usb_t *)
{
    return 0;
}

bool candle_usb_control(candle_usb_t *usb, uint8_t request, uint8_t requesttype,
                        uint16_t value, uint16_t index, void *data, uint16_t size)
{
    return usb->dev->control(request, requesttype, value, index, static_cast<uint8_t *>(data), size);
}

bool candle_usb_write(candle_usb_t *usb, const void *data, uint32_t len, uint32_t)
{
    return usb->dev->bulkOut(static_cast<const uint8_t *>(data), len);
}

candle_err_t candle_usb_read(candle_usb_t *usb, uint8_t *buf, uint32_t bufsize,
                             uint32_t *len, uint32_t timeout_ms)
{
    if (!usb->reading)
        return CANDLE_ERR_READ_WAIT;
    switch (usb->dev->bulkIn(buf, bufsize, len, timeout_ms))
    {
    case 0: return CANDLE_ERR_OK;
    case 1: return CANDLE_ERR_READ_TIMEOUT;
    default: return CANDLE_ERR_READ_RESULT;  // the transfer failed, as WinUSB does when unplugged
    }
}

} // extern "C"
