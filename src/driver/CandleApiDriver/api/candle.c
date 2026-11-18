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

#include "candle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "candle_defs.h"
#include "candle_ctrl_req.h"
#include "candle_usb.h"
#include "ch_9.h"

static bool candle_dev_interal_open(candle_device_t *dev, bool start_reads);

candle_log_fn_t candle_log_fn = NULL;
bool candle_log_verbose = false;

static void candle_vlogf(const wchar_t *fmt, va_list args)
{
    wchar_t buf[512];
    if (vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, args) >= 0) {
        candle_log_fn(buf);
    }
}

void candle_logf(const wchar_t *fmt, ...)
{
    if (candle_log_fn == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    candle_vlogf(fmt, args);
    va_end(args);
}

void candle_logf_verbose(const wchar_t *fmt, ...)
{
    if (candle_log_fn == NULL || !candle_log_verbose) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    candle_vlogf(fmt, args);
    va_end(args);
}

bool __stdcall candle_list_scan(candle_list_handle *list)
{
    if (list == NULL) {
        return false;
    }

    candle_list_t *l = (candle_list_t *)calloc(1, sizeof(candle_list_t));
    *list = l;
    if (l == NULL) {
        return false;
    }

    wchar_t (*paths)[CANDLE_USB_PATH_LEN] = calloc(CANDLE_MAX_DEVICES, sizeof(*paths));
    if (paths == NULL) {
        l->last_error = CANDLE_ERR_MALLOC;
        return false;
    }
    candle_err_t err = CANDLE_ERR_OK;
    const int n = candle_usb_scan(paths, CANDLE_MAX_DEVICES, &err);
    if (n < 0) {
        free(paths);
        l->last_error = err;
        return false;
    }

    for (int i = 0; i < n; i++) {
        candle_device_t *dev = &l->dev[i];
        wcsncpy(dev->path, paths[i], CANDLE_USB_PATH_LEN - 1);
        dev->path[CANDLE_USB_PATH_LEN - 1] = 0;

        /* try to open to read device infos and see if it is avail */
        if (candle_dev_interal_open(dev, false)) {
            dev->state = CANDLE_DEVSTATE_AVAIL;
            candle_dev_close(dev);
        } else {
            dev->state = CANDLE_DEVSTATE_INUSE;
        }
        dev->last_error = CANDLE_ERR_OK;
    }

    free(paths);
    l->num_devices = (uint8_t)n;
    l->last_error  = CANDLE_ERR_OK;
    return true;
}

bool __stdcall DLL candle_list_free(candle_list_handle list)
{
    free(list);
    return true;
}

bool __stdcall DLL candle_list_length(candle_list_handle list, uint8_t *len)
{
    candle_list_t *l = (candle_list_t *)list;
    *len = l->num_devices;
    return true;
}

bool __stdcall DLL candle_dev_get(candle_list_handle list, uint8_t dev_num, candle_handle *hdev)
{
    candle_list_t *l = (candle_list_t *)list;
    if (l==NULL) {
        return false;
    }

    if (dev_num >= CANDLE_MAX_DEVICES) {
        l->last_error = CANDLE_ERR_DEV_OUT_OF_RANGE;
        return false;
    }

    candle_device_t *dev = calloc(1, sizeof(candle_device_t));
    *hdev = dev;
    if (dev==NULL) {
        l->last_error = CANDLE_ERR_MALLOC;
        return false;
    }

    memcpy(dev, &l->dev[dev_num], sizeof(candle_device_t));
    l->last_error = CANDLE_ERR_OK;
    dev->last_error = CANDLE_ERR_OK;
    return true;
}


bool __stdcall DLL candle_dev_get_state(candle_handle hdev, candle_devstate_t *state)
{
    if (hdev==NULL) {
        return false;
    } else {
        candle_device_t *dev = (candle_device_t*)hdev;
        *state = dev->state;
        return true;
    }
}

wchar_t __stdcall DLL *candle_dev_get_path(candle_handle hdev)
{
    if (hdev==NULL) {
        return NULL;
    } else {
        candle_device_t *dev = (candle_device_t*)hdev;
        return dev->path;
    }
}

static bool candle_dev_interal_open(candle_device_t *dev, bool start_reads)
{
    candle_err_t err = CANDLE_ERR_OK;
    dev->usb = candle_usb_open(dev->path, start_reads, &err);
    if (dev->usb == NULL) {
        dev->last_error = err;
        return false;
    }
    dev->interfaceNumber = candle_usb_interface_number(dev->usb);

    if (!candle_ctrl_set_host_format(dev)) {
        goto close_usb;
    }

    if (!candle_ctrl_get_config(dev, &dev->dconf)) {
        goto close_usb;
    }
    dev->has_dconf = true;
    candle_logf(L"device config channels=%u sw=0x%08x hw=0x%08x",
                dev->dconf.icount + 1,
                dev->dconf.sw_version,
                dev->dconf.hw_version);

    if (!candle_ctrl_get_capability(dev, 0, &dev->bt_const)) {
        dev->last_error = CANDLE_ERR_GET_BITTIMING_CONST;
        goto close_usb;
    }
    candle_logf(L"cap ch0 feature=0x%08x fclk=%u tseg1=%u..%u tseg2=%u..%u sjw=%u brp=%u..%u inc=%u",
                dev->bt_const.feature,
                dev->bt_const.fclk_can,
                dev->bt_const.tseg1_min,
                dev->bt_const.tseg1_max,
                dev->bt_const.tseg2_min,
                dev->bt_const.tseg2_max,
                dev->bt_const.sjw_max,
                dev->bt_const.brp_min,
                dev->bt_const.brp_max,
                dev->bt_const.brp_inc);

    /* Query capabilities for each channel on multi-channel devices */
    uint8_t num_channels = dev->dconf.icount + 1;
    if (num_channels > 8) num_channels = 8;
    for (uint8_t ch = 0; ch < num_channels; ch++) {
        if (!candle_ctrl_get_capability(dev, ch, &dev->ch_caps[ch])) {
            /* Fall back to channel 0 capabilities for this channel */
            memcpy(&dev->ch_caps[ch], &dev->bt_const, sizeof(candle_capability_t));
            candle_logf(L"cap ch%u failed, falling back to ch0", ch);
        } else {
            candle_logf(L"cap ch%u feature=0x%08x fclk=%u",
                        ch,
                        dev->ch_caps[ch].feature,
                        dev->ch_caps[ch].fclk_can);
        }
    }

    dev->last_error = CANDLE_ERR_OK;
    return true;

close_usb:
    candle_usb_close(dev->usb);
    dev->usb = NULL;
    return false;
}

bool __stdcall DLL candle_dev_open(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_dev_interal_open(dev, true); // keeps last_error on failure
}

bool __stdcall DLL candle_dev_get_timestamp_us(candle_handle hdev, uint32_t *timestamp_us)
{
	return candle_ctrl_get_timestamp(hdev, timestamp_us);
}

bool __stdcall DLL candle_dev_get_version(candle_handle hdev, uint32_t *sw_version, uint32_t *hw_version)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    if (!dev->has_dconf) {
        return false;
    }
    *sw_version = dev->dconf.sw_version;
    *hw_version = dev->dconf.hw_version;
    return true;
}

bool __stdcall DLL candle_dev_close(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;

    candle_usb_close(dev->usb);
    dev->usb = NULL;

    dev->last_error = CANDLE_ERR_OK;
    return true;
}

bool __stdcall DLL candle_dev_free(candle_handle hdev)
{
    free(hdev);
    return true;
}

candle_err_t __stdcall DLL candle_dev_last_error(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return dev->last_error;
}

bool __stdcall DLL candle_channel_count(candle_handle hdev, uint8_t *num_channels)
{
    // TODO check if info was already read from device; try to do so; throw error...
    candle_device_t *dev = (candle_device_t*)hdev;
    *num_channels = dev->dconf.icount+1;
    return true;
}

bool __stdcall DLL candle_channel_get_capabilities(candle_handle hdev, uint8_t ch, candle_capability_t *cap)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    uint8_t num_channels = dev->dconf.icount + 1;
    if (ch < num_channels && ch < 8) {
        memcpy(cap, &dev->ch_caps[ch], sizeof(candle_capability_t));
    } else {
        memcpy(cap, &dev->bt_const, sizeof(candle_capability_t));
    }
    return true;
}

bool __stdcall DLL candle_channel_get_state(candle_handle hdev, uint8_t ch, candle_can_state_t *state)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    candle_device_state_t ds;
    if (!candle_ctrl_get_state(dev, ch, &ds)) {
        return false;
    }
    *state = (candle_can_state_t)ds.state;
    return true;
}

bool __stdcall DLL candle_channel_set_tdc_auto(candle_handle hdev, uint8_t ch)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    candle_device_tdc_t tdc = { 0, 0, 0, 1u << 1 }; /* GS_CAN_TDC_MODE_AUTO: firmware computes TDCO */
    return candle_ctrl_set_tdc(dev, ch, &tdc);
}

bool __stdcall DLL candle_channel_bus_off_recovery(candle_handle hdev, uint8_t ch)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_bus_off_recovery(dev, ch);
}

bool __stdcall DLL candle_channel_set_timing(candle_handle hdev, uint8_t ch, candle_bittiming_t *data)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_set_bittiming(dev, ch, data);
}

bool __stdcall DLL candle_channel_set_bitrate(candle_handle hdev, uint8_t ch, uint32_t bitrate)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;

    if (dev->bt_const.fclk_can != 48000000) {
        /* this function only works for the candleLight base clock of 48MHz */
        dev->last_error = CANDLE_ERR_BITRATE_FCLK;
        return false;
    }

    candle_bittiming_t t;
    t.prop_seg = 1;
    t.sjw = 1;
    t.phase_seg1 = 13 - t.prop_seg;
    t.phase_seg2 = 2;

    switch (bitrate) {
        case 10000:
            t.brp = 300;
            break;

        case 20000:
            t.brp = 150;
            break;

        case 50000:
            t.brp = 60;
            break;

        case 83333:
            t.brp = 36;
            break;

        case 100000:
            t.brp = 30;
            break;

        case 125000:
            t.brp = 24;
            break;

        case 250000:
            t.brp = 12;
            break;

        case 500000:
            t.brp = 6;
            break;

        case 800000:
            t.brp = 4;
            t.phase_seg1 = 12 - t.prop_seg;
            t.phase_seg2 = 2;
            break;

        case 1000000:
            t.brp = 3;
            break;

        default:
            dev->last_error = CANDLE_ERR_BITRATE_UNSUPPORTED;
            return false;
    }

    return candle_ctrl_set_bittiming(dev, ch, &t);
}

bool __stdcall DLL candle_channel_start(candle_handle hdev, uint8_t ch, uint32_t flags)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;
    candle_capability_t *cap = (ch < 8) ? &dev->ch_caps[ch] : &dev->bt_const;

    if (cap->feature & CANDLE_FEATURE_HW_TIMESTAMP) {
        flags |= CANDLE_MODE_HW_TIMESTAMP;
    } else {
        candle_logf(L"channel %u has no HW timestamp capability; starting without timestamp flag", ch);
    }

    bool rc = candle_ctrl_set_device_mode(dev, ch, CANDLE_DEVMODE_START, flags);
    candle_logf(L"channel %u start flags=0x%08x result=%u err=%u",
                ch,
                flags,
                rc ? 1 : 0,
                dev->last_error);
    return rc;
}

bool __stdcall DLL candle_channel_stop(candle_handle hdev, uint8_t ch)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_set_device_mode(dev, ch, CANDLE_DEVMODE_RESET, 0);
}

/* One bulk OUT transfer, aborted after 150 ms (CandleApiInterface waits at
 * most 200 ms for the write lock of a shared device). */
static bool candle_write_frame(candle_device_t *dev, const void *buf, uint32_t len)
{
    if (dev->usb == NULL) {
        return false;
    }
    return candle_usb_write(dev->usb, buf, len, 150);
}

/* One bulk IN transfer into buf; sets dev->last_error and returns false on
 * timeout or failure. */
static bool candle_read_transfer(candle_device_t *dev, uint8_t *buf, uint32_t bufsize, uint32_t *len, uint32_t timeout_ms)
{
    if (dev->usb == NULL) {
        dev->last_error = CANDLE_ERR_READ_WAIT;
        return false;
    }
    candle_err_t err = candle_usb_read(dev->usb, buf, bufsize, len, timeout_ms);
    dev->last_error = err;
    return err == CANDLE_ERR_OK;
}

bool __stdcall DLL candle_frame_send(candle_handle hdev, uint8_t ch, candle_frame_t *frame)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    frame->echo_id = 0;
    frame->channel = ch;
    bool rc = candle_write_frame(dev, frame, sizeof(*frame));
    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SEND_FRAME;
    return rc;
}

bool __stdcall DLL candle_frame_read(candle_handle hdev, candle_frame_t *frame, uint32_t timeout_ms)
{
    // TODO ensure device is open..
    candle_device_t *dev = (candle_device_t*)hdev;

    uint8_t buf[512];
    uint32_t bytes_transfered = 0;
    if (!candle_read_transfer(dev, buf, sizeof(buf), &bytes_transfered, timeout_ms)) {
        return false;
    }

    if (bytes_transfered < sizeof(*frame)-4) {
        candle_logf(L"classic read too small bytes=%u min=%u",
                    (unsigned)bytes_transfered,
                    (unsigned)(sizeof(*frame) - 4));
        dev->last_error = CANDLE_ERR_READ_SIZE;
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    uint32_t copy_len = (bytes_transfered < sizeof(*frame)) ? bytes_transfered : (uint32_t)sizeof(*frame);
    memcpy(frame, buf, copy_len);
    candle_logf_verbose(L"classic read bytes=%u echo=0x%08x can_id=0x%08x dlc=%u ch=%u flags=0x%02x ts=%u",
                (unsigned)bytes_transfered,
                frame->echo_id,
                frame->can_id,
                frame->can_dlc,
                frame->channel,
                frame->flags,
                frame->timestamp_us);

    dev->last_error = CANDLE_ERR_OK;
    return true;
}

candle_frametype_t __stdcall DLL candle_frame_type(candle_frame_t *frame)
{
    if (frame->echo_id != 0xFFFFFFFF) {
        return CANDLE_FRAMETYPE_ECHO;
    };

    if (frame->can_id & CANDLE_ID_ERR) {
        return CANDLE_FRAMETYPE_ERROR;
    }

    return CANDLE_FRAMETYPE_RECEIVE;
}

uint32_t __stdcall DLL candle_frame_id(candle_frame_t *frame)
{
    return frame->can_id & 0x1FFFFFFF;
}

bool __stdcall DLL candle_frame_is_extended_id(candle_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_EXTENDED) != 0;
}

bool __stdcall DLL candle_frame_is_rtr(candle_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_RTR) != 0;
}

uint8_t __stdcall DLL candle_frame_dlc(candle_frame_t *frame)
{
    return frame->can_dlc;
}

uint8_t __stdcall DLL *candle_frame_data(candle_frame_t *frame)
{
    return frame->data;
}

uint32_t __stdcall DLL candle_frame_timestamp_us(candle_frame_t *frame)
{
    return frame->timestamp_us;
}

/* ---- CAN FD extensions ---- */

bool __stdcall DLL candle_channel_set_data_timing(candle_handle hdev, uint8_t ch, candle_bittiming_t *data)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_set_data_bittiming(dev, ch, data);
}

bool __stdcall DLL candle_fd_frame_send(candle_handle hdev, uint8_t ch, candle_fd_frame_t *frame)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    frame->echo_id = 0;
    frame->channel = ch;
    bool rc = candle_write_frame(dev, frame, sizeof(*frame));
    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SEND_FRAME;
    return rc;
}

bool __stdcall DLL candle_fd_frame_read(candle_handle hdev, candle_fd_frame_t *frame, uint32_t timeout_ms)
{
    candle_device_t *dev = (candle_device_t*)hdev;

    uint8_t buf[512];
    uint32_t bytes_transfered = 0;
    if (!candle_read_transfer(dev, buf, sizeof(buf), &bytes_transfered, timeout_ms)) {
        return false;
    }

    /* Minimum: classic CAN header (12 bytes) + 8 data bytes = 20 bytes */
    static const uint32_t classic_min = sizeof(candle_frame_t) - 4;
    if (bytes_transfered < classic_min) {
        candle_logf(L"fd read too small bytes=%u min=%u",
                    (unsigned)bytes_transfered,
                    (unsigned)classic_min);
        dev->last_error = CANDLE_ERR_READ_SIZE;
        return false;
    }

    memset(frame, 0, sizeof(*frame));

    /*
     * Detect frame type from the flags byte (offset 10 in both structs).
     * Classic CAN frames: header(12) + data(8) + timestamp(4) = 24 bytes total.
     *
     * FD frames come in two wire formats:
     *  - Legacy fixed (candleLight/CANable 1.x): always 80 bytes — header(12) +
     *    data[64] + timestamp(4).  The timestamp is ALWAYS at offset 76, regardless
     *    of the actual DLC.  Identified by bytes_transferred == sizeof(candle_fd_frame_t).
     *  - Variable-length (CANnectivity/Zephyr): header(12) + actual_data(DLC) +
     *    timestamp(4).  Identified by bytes_transferred < sizeof(candle_fd_frame_t).
     */
    bool is_fd_frame = (buf[10] & CANDLE_FRAME_FLAG_FD) != 0;

    if (is_fd_frame) {
        /* can_dlc is at byte offset 8 in both classic and FD wire frames. */
        const uint8_t raw_dlc  = buf[8];
        const uint32_t data_len = candle_dlc_to_len(raw_dlc);
        const uint32_t min_size = 12 + data_len; /* header + data, without timestamp */

        if (bytes_transfered < min_size) {
            candle_logf(L"fd read FD frame too small bytes=%u min=%u flags=0x%02x dlc=%u",
                        (unsigned)bytes_transfered,
                        (unsigned)min_size,
                        buf[10],
                        raw_dlc);
            dev->last_error = CANDLE_ERR_READ_SIZE;
            return false;
        }

        /* Copy the fixed 12-byte header (echo_id … reserved). */
        memcpy(frame, buf, 12);
        /* Copy data at offset 12 into the struct's data field. */
        memcpy(frame->data, buf + 12, data_len);

        /* Timestamp location depends on the wire format (see comment above). */
        const uint32_t fixed_ts_offset = (uint32_t)(sizeof(candle_fd_frame_t) - sizeof(uint32_t)); /* = 76 */
        const uint32_t ts_offset = (bytes_transfered >= (uint32_t)sizeof(candle_fd_frame_t))
                                   ? fixed_ts_offset
                                   : min_size;
        if (bytes_transfered >= ts_offset + (uint32_t)sizeof(uint32_t)) {
            memcpy(&frame->timestamp_us, buf + ts_offset, sizeof(uint32_t));
        }
        /* else: timestamp stays zero from memset above */
    } else {
        /* Classic CAN frame — copy into FD struct, fixing the timestamp position */
        candle_frame_t classic;
        uint32_t copy_len = (bytes_transfered < sizeof(classic)) ? bytes_transfered : (uint32_t)sizeof(classic);
        memset(&classic, 0, sizeof(classic));
        memcpy(&classic, buf, copy_len);

        frame->echo_id      = classic.echo_id;
        frame->can_id       = classic.can_id;
        frame->can_dlc      = classic.can_dlc;
        frame->channel      = classic.channel;
        frame->flags        = classic.flags;
        frame->reserved     = classic.reserved;
        memcpy(frame->data, classic.data, 8);
        frame->timestamp_us = (bytes_transfered >= sizeof(classic)) ? classic.timestamp_us : 0;
    }
    candle_logf_verbose(L"fd read bytes=%u is_fd=%u echo=0x%08x can_id=0x%08x dlc=%u ch=%u flags=0x%02x ts=%u",
                (unsigned)bytes_transfered,
                is_fd_frame ? 1 : 0,
                frame->echo_id,
                frame->can_id,
                frame->can_dlc,
                frame->channel,
                frame->flags,
                frame->timestamp_us);

    dev->last_error = CANDLE_ERR_OK;
    return true;
}

candle_frametype_t __stdcall DLL candle_fd_frame_type(candle_fd_frame_t *frame)
{
    if (frame->echo_id != 0xFFFFFFFF) {
        return CANDLE_FRAMETYPE_ECHO;
    }
    if (frame->can_id & CANDLE_ID_ERR) {
        return CANDLE_FRAMETYPE_ERROR;
    }
    return CANDLE_FRAMETYPE_RECEIVE;
}

uint32_t __stdcall DLL candle_fd_frame_id(candle_fd_frame_t *frame)
{
    return frame->can_id & 0x1FFFFFFF;
}

bool __stdcall DLL candle_fd_frame_is_extended_id(candle_fd_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_EXTENDED) != 0;
}

bool __stdcall DLL candle_fd_frame_is_rtr(candle_fd_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_RTR) != 0;
}

bool __stdcall DLL candle_fd_frame_is_fd(candle_fd_frame_t *frame)
{
    return (frame->flags & CANDLE_FRAME_FLAG_FD) != 0;
}

bool __stdcall DLL candle_fd_frame_is_brs(candle_fd_frame_t *frame)
{
    return (frame->flags & CANDLE_FRAME_FLAG_BRS) != 0;
}

uint8_t __stdcall DLL candle_fd_frame_dlc(candle_fd_frame_t *frame)
{
    return frame->can_dlc;
}

uint8_t __stdcall DLL *candle_fd_frame_data(candle_fd_frame_t *frame)
{
    return frame->data;
}

uint32_t __stdcall DLL candle_fd_frame_timestamp_us(candle_fd_frame_t *frame)
{
    return frame->timestamp_us;
}
