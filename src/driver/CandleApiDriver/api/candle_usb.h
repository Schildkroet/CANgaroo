/*

  Copyright (c) 2026 Schildkroet

  This file is part of the candle windows API.

  This library is free software: you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library.  If not, see <http://www.gnu.org/licenses/>.

*/

#pragma once

// USB access below the gs_usb protocol of candle.c / candle_ctrl_req.c.
//
// candle.c speaks gs_usb (host format, device mode, bit timing, host frames)
// and needs only the transfers declared here. Implementations:
//   candle_usb_winusb.c   WinUSB + SetupAPI, used by the application on Windows
//   tests/candle_driver   a simulated gs_usb device (FakeGsUsbDevice)
//   tests/hw_compare      libusb, to run candle.c against a real adapter on Linux

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include "candle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANDLE_USB_PATH_LEN 256

typedef struct candle_usb candle_usb_t;

// Writes up to `max` device paths of gs_usb interfaces present. Returns the
// number found, or -1 with *err set if enumeration itself failed.
int candle_usb_scan(wchar_t paths[][CANDLE_USB_PATH_LEN], int max, candle_err_t *err);

// Opens the gs_usb interface at `path`. With start_reads, bulk IN transfers
// are started so candle_usb_read() can return frames. NULL with *err set on
// failure (e.g. CANDLE_ERR_CREATE_FILE while another program has it open).
candle_usb_t *candle_usb_open(const wchar_t *path, bool start_reads, candle_err_t *err);
void candle_usb_close(candle_usb_t *usb);

// bInterfaceNumber, the wIndex of the vendor control requests
uint8_t candle_usb_interface_number(const candle_usb_t *usb);

// Vendor control transfer; false if the device stalled it or the transfer failed.
bool candle_usb_control(candle_usb_t *usb, uint8_t request, uint8_t requesttype,
                        uint16_t value, uint16_t index, void *data, uint16_t size);

// One bulk OUT transfer; false on error or after timeout_ms.
bool candle_usb_write(candle_usb_t *usb, const void *data, uint32_t len, uint32_t timeout_ms);

// One bulk IN transfer of at most bufsize bytes. CANDLE_ERR_OK with *len set,
// CANDLE_ERR_READ_TIMEOUT if nothing arrived in timeout_ms, otherwise
// CANDLE_ERR_READ_RESULT (or READ_WAIT) for a failed transfer.
candle_err_t candle_usb_read(candle_usb_t *usb, uint8_t *buf, uint32_t bufsize,
                             uint32_t *len, uint32_t timeout_ms);

// Logging into candle_log_fn, shared by candle.c and the implementations
void candle_logf(const wchar_t *fmt, ...);
void candle_logf_verbose(const wchar_t *fmt, ...);

#ifdef __cplusplus
}
#endif
