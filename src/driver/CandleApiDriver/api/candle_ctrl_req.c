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

#include "candle_ctrl_req.h"
#include "ch_9.h"


enum {
    CANDLE_BREQ_HOST_FORMAT    = 0,
    CANDLE_BREQ_BITTIMING      = 1,
    CANDLE_BREQ_MODE           = 2,
    CANDLE_BREQ_BERR           = 3,
    CANDLE_BREQ_BT_CONST       = 4,
    CANDLE_BREQ_DEVICE_CONFIG  = 5,
    CANDLE_TIMESTAMP_GET       = 6,
    /* 7: IDENTIFY, 8: GET_USER_ID, 9: SET_USER_ID (not used here) */
    CANDLE_BREQ_DATA_BITTIMING = 10,
    /* 11: BT_CONST_EXT, 12: SET_TERMINATION, 13: GET_TERMINATION (not used here);
     * numbering as in enum gs_usb_breq, Linux drivers/net/can/usb/gs_usb.c */
    CANDLE_BREQ_GET_STATE      = 14,
    /* 15, 16: SET/GET_FILTER, 17: GET_TDC_CONST, 19: GET_TDC (not used here) */
    CANDLE_BREQ_SET_TDC        = 18,
    CANDLE_BREQ_BUS_OFF_RECOVERY = 32,
};

// Vendor control transfer to the gs_usb interface; logged when verbose.
static bool usb_control_msg(candle_device_t *dev, uint8_t request, uint8_t requesttype, uint16_t value, uint16_t index, void *data, uint16_t size)
{
    /* Closed (or never opened): there is no handle to transfer on. Callers
     * outside the measurement, e.g. a status poll, must get an error. */
    if (dev->usb == NULL) {
        dev->last_error = CANDLE_ERR_NOT_OPEN;
        return false;
    }
    bool rc = candle_usb_control(dev->usb, request, requesttype, value, index, data, size);
    candle_logf_verbose(L"ctrl req=0x%02x type=0x%02x value=%u index=%u size=%u rc=%u",
                        request,
                        requesttype,
                        value,
                        index,
                        size,
                        rc ? 1 : 0);
    return rc;
}

bool candle_ctrl_set_host_format(candle_device_t *dev)
{
    candle_host_config_t hconf;
    hconf.byte_order = 0x0000beef;

    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_HOST_FORMAT,
        USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        1,
        dev->interfaceNumber,
        &hconf,
        sizeof(hconf)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SET_HOST_FORMAT;
    return rc;
}

bool candle_ctrl_set_device_mode(candle_device_t *dev, uint8_t channel, uint32_t mode, uint32_t flags)
{
    candle_device_mode_t dm;
    dm.mode = mode;
    dm.flags = flags;

    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_MODE,
        USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        dev->interfaceNumber,
        &dm,
        sizeof(dm)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SET_DEVICE_MODE;
    return rc;
}


bool candle_ctrl_get_config(candle_device_t *dev, candle_device_config_t *dconf)
{
    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_DEVICE_CONFIG,
        USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        1,
        dev->interfaceNumber,
        dconf,
        sizeof(*dconf)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_GET_DEVICE_INFO;
    return rc;
}

bool candle_ctrl_get_timestamp(candle_device_t *dev, uint32_t *current_timestamp)
{
    /* wValue is a channel number for this request in candleLight
     * (USBD_GS_CAN_Config_Request: only HOST_FORMAT and DEVICE_CONFIG are per
     * device): 1 stalls on a single-channel board. Channel 0 always exists. */
    bool rc = usb_control_msg(
        dev,
        CANDLE_TIMESTAMP_GET,
        USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        0,
        dev->interfaceNumber,
        current_timestamp,
        sizeof(*current_timestamp)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_GET_TIMESTAMP;
    return rc;
}

bool candle_ctrl_get_capability(candle_device_t *dev, uint8_t channel, candle_capability_t *data)
{
    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_BT_CONST,
        USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        0,
        data,
        sizeof(*data)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_GET_BITTIMING_CONST;
    return rc;
}

bool candle_ctrl_set_bittiming(candle_device_t *dev, uint8_t channel, candle_bittiming_t *data)
{
    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_BITTIMING,
        USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        0,
        data,
        sizeof(*data)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SET_BITTIMING;
    return rc;
}

bool candle_ctrl_set_data_bittiming(candle_device_t *dev, uint8_t channel, candle_bittiming_t *data)
{
    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_DATA_BITTIMING,
        USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        0,
        data,
        sizeof(*data)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SET_BITTIMING;
    return rc;
}

bool candle_ctrl_set_tdc(candle_device_t *dev, uint8_t channel, candle_device_tdc_t *data)
{
    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_SET_TDC,
        USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        dev->interfaceNumber,
        data,
        sizeof(*data)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SET_BITTIMING;
    return rc;
}

bool candle_ctrl_bus_off_recovery(candle_device_t *dev, uint8_t channel)
{
    candle_device_bus_off_recovery_t data = { 0 };

    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_BUS_OFF_RECOVERY,
        USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        dev->interfaceNumber,
        &data,
        sizeof(data)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SET_DEVICE_MODE;
    return rc;
}

bool candle_ctrl_get_state(candle_device_t *dev, uint8_t channel, candle_device_state_t *data)
{
    bool rc = usb_control_msg(
        dev,
        CANDLE_BREQ_GET_STATE,
        USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_INTERFACE,
        channel,
        dev->interfaceNumber,
        data,
        sizeof(*data)
    );

    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_GET_DEVICE_INFO;
    return rc;
}
