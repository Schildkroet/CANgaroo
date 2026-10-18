/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "candle_defs.h"

enum {
    CANDLE_DEVMODE_RESET = 0,
    CANDLE_DEVMODE_START = 1
};

#pragma pack(push, 1)
typedef struct {
    uint32_t state;
    uint32_t rxerr;
    uint32_t txerr;
} candle_device_state_t;

typedef struct {
    uint32_t unused;
} candle_device_bus_off_recovery_t;

typedef struct {
    uint32_t tdcv;
    uint32_t tdco;
    uint32_t tdcf;
    uint32_t mode;
} candle_device_tdc_t;
#pragma pack(pop)

bool candle_ctrl_set_host_format(candle_device_t *dev);
bool candle_ctrl_set_device_mode(candle_device_t *dev, uint8_t channel, uint32_t mode, uint32_t flags);
bool candle_ctrl_get_config(candle_device_t *dev, candle_device_config_t *dconf);
bool candle_ctrl_get_capability(candle_device_t *dev, uint8_t channel, candle_capability_t *data);
bool candle_ctrl_set_bittiming(candle_device_t *dev, uint8_t channel, candle_bittiming_t *data);
bool candle_ctrl_set_data_bittiming(candle_device_t *dev, uint8_t channel, candle_bittiming_t *data);
bool candle_ctrl_get_timestamp(candle_device_t *dev, uint32_t *current_timestamp);
bool candle_ctrl_set_tdc(candle_device_t *dev, uint8_t channel, candle_device_tdc_t *data);
bool candle_ctrl_bus_off_recovery(candle_device_t *dev, uint8_t channel);
bool candle_ctrl_get_state(candle_device_t *dev, uint8_t channel, candle_device_state_t *data);

