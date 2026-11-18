/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>

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

// gs_usb protocol state of a device. Platform-free: the USB handles live
// behind candle_usb_t (candle_usb.h).

#include <stdint.h>

#include "candle.h"
#include "candle_usb.h"

#define CANDLE_MAX_DEVICES 32

#pragma pack(push,1)

typedef struct {
    uint32_t byte_order;
} candle_host_config_t;

typedef struct {
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
    uint8_t icount;
    uint32_t sw_version;
    uint32_t hw_version;
} candle_device_config_t;

typedef struct {
    uint32_t mode;
    uint32_t flags;
} candle_device_mode_t;

#pragma pack(pop)

typedef struct {
    wchar_t path[CANDLE_USB_PATH_LEN];
    candle_devstate_t state;
    candle_err_t last_error;

    candle_usb_t *usb;  // NULL while closed
    uint8_t interfaceNumber;

    candle_device_config_t dconf;
    bool has_dconf;  // dconf was read from the device; kept across close
    candle_capability_t bt_const;
    /* Per-channel capabilities: index 0..dconf.icount, maximum 8 channels */
    candle_capability_t ch_caps[8];
} candle_device_t;

typedef struct {
    uint8_t num_devices;
    candle_err_t last_error;
    candle_device_t dev[CANDLE_MAX_DEVICES];
} candle_list_t;
