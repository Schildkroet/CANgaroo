/*
 * gs_usb.c — TinyUSB custom class driver for the gs_usb CAN protocol.
 *
 * Copyright (c) 2026 Schildkroet
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Registers itself as an application-level class driver via
 * usbd_app_driver_get_cb() and handles:
 *   - Vendor control requests (bittiming, mode, device config, …)
 *   - Bulk OUT: host → device (CAN TX frames, channel field selects FDCAN)
 *   - Bulk IN:  device → host (CAN RX + TX echo, channel field set by driver)
 *
 * Compatible with the Linux gs_usb kernel driver and tools such as
 * can-utils (candump, cansend) and python-can.
 *
 * USB IDs: 0x1d50 / 0x606f  (OpenMoko / Candlelight — recognized by
 * the Linux gs_usb driver without any extra configuration.)
 *
 * The number of exposed CAN channels is controlled by GS_USB_CAN_CHANNEL_COUNT
 * in gs_usb_config.h.  Classic CAN and CAN FD (BREQ_DATA_BITTIMING /
 * BREQ_BT_CONST_EXT, 76/80-byte frames spanning two bulk packets) are both
 * supported; the CAN engine behind the gs_engine_* hooks does the actual work.
 */

#include "gs_usb.h"
#include "gs_usb_config.h"
#include "device/usbd_pvt.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// The wire layout must match the Linux gs_usb driver (struct gs_host_frame with
// its classic_can_ts / canfd_ts members).  Catch any accidental change at
// compile time.
TU_VERIFY_STATIC(sizeof(gs_host_frame_t) == GS_HOST_FRAME_SIZE_FD_TS, "gs_host_frame_t must be 80 bytes");
TU_VERIFY_STATIC(offsetof(gs_host_frame_t, classic.timestamp_us) == GS_HOST_FRAME_SIZE, "classic timestamp must follow data[8]");
TU_VERIFY_STATIC(offsetof(gs_host_frame_t, fd.timestamp_us) == GS_HOST_FRAME_SIZE_FD, "FD timestamp must follow data[64]");
TU_VERIFY_STATIC(sizeof(gs_device_bt_const_ext_t) == 72, "gs_device_bt_const_ext_t must be 72 bytes");

//--------------------------------------------------------------------+
// Critical-section helpers
//
// The IN queue and _in_busy flag are touched from three contexts: the CAN
// engine (gs_usb_report_frame(), typically an FDCAN RX interrupt), the USB
// stack (gsusb_xfer_cb()) and the main loop (gs_usb_task()).  All accesses to
// that shared state must be atomic, so wrap them in a PRIMASK save/restore
// critical section (nesting-safe).
//--------------------------------------------------------------------+
static inline uint32_t cs_enter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void cs_exit(uint32_t primask)
{
    __set_PRIMASK(primask);
}

//--------------------------------------------------------------------+
// This driver is pure USB plumbing.
//
// It relays host vendor requests to the CAN engine (the gs_engine_* hooks,
// weak no-op stubs at the bottom of this file) and streams frames the engine
// reports back — via gs_usb_report_frame() — to the host over bulk IN.
//
// All CAN bit-timing application, mode control, TX and RX live in the engine,
// which is integrated separately and overrides the weak hooks.  Nothing in
// this file touches the FDCAN peripheral.
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// USB interface state (shared across all channels)
//--------------------------------------------------------------------+
static struct
{
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t rhport;
} _usb;

// Channel index saved during SETUP stage, consumed during DATA stage
static uint8_t _ctrl_ch;

// Shared buffer for control transfers (one at a time on EP0)
static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN union
{
    gs_host_config_t      host_config;
    gs_device_config_t    device_config;
    gs_device_bt_const_t     bt_const;
    gs_device_bt_const_ext_t bt_const_ext;
    gs_device_bittiming_t    bittiming;
    gs_device_mode_t         mode;
    gs_device_state_t        state;
    uint32_t                 timestamp;
    uint8_t                  raw[80];
} _ctrl_buf;

// Bulk OUT buffer.  Hosts differ in the frame length they send (Linux gs_usb:
// 20 bytes, 76 in FD mode; Windows candle_api: 24 / 80 bytes incl.
// timestamp_us).  A CAN FD frame spans two 64-byte packets, and the fsdev port
// copies each whole received packet regardless of the requested length, so
// the buffer holds two full packets.
#define GS_USB_OUT_BUF_SIZE 128u

static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN union
{
    gs_host_frame_t frame;
    uint8_t         raw[GS_USB_OUT_BUF_SIZE];
} _out_buf;

// IN queue (RX frames, TX echoes, error frames); usable capacity is one less
#define IN_QUEUE_SIZE 32u

// Slots only TX echoes and error frames may use: when the host falls behind,
// bus RX frames are dropped first, since a lost echo stalls the Linux driver's
// transmit path.
#define IN_QUEUE_RESERVED 8u

static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN gs_host_frame_t _in_buf;
static gs_host_frame_t _in_queue[IN_QUEUE_SIZE];
static uint8_t _in_head;
static uint8_t _in_tail;
static bool    _in_busy;

// true while the OUT endpoint is armed for the next host CAN TX frame
static bool _out_armed;

// true while _out_buf holds a host frame the engine could not accept yet; the
// OUT endpoint stays un-armed (host NAKed) until gs_usb_task() hands it over.
static bool _out_pending;

// Per channel: host started it with GS_CAN_FLAG_HW_TIMESTAMP, so IN frames
// for that channel carry the 4-byte timestamp_us trailer.
static bool _hw_timestamp[GS_USB_CAN_CHANNEL_COUNT];

//--------------------------------------------------------------------+
// IN queue helpers
//--------------------------------------------------------------------+

// Push a frame, but only if more than `reserve` slots are free.
static bool in_queue_push(const gs_host_frame_t *frame, uint8_t reserve)
{
    uint32_t pm = cs_enter();
    uint8_t used  = (uint8_t)((_in_head + IN_QUEUE_SIZE - _in_tail) % IN_QUEUE_SIZE);
    uint8_t avail = (uint8_t)(IN_QUEUE_SIZE - 1u - used);
    if (avail <= reserve)
    {
        cs_exit(pm);
        return false;
    }
    uint8_t next = (uint8_t)((_in_head + 1u) % IN_QUEUE_SIZE);
    _in_queue[_in_head] = *frame;
    _in_head = next;
    cs_exit(pm);
    return true;
}

static void in_try_send(void)
{
    // Before SET_CONFIGURATION (and right after a bus reset) ep_in is 0: a
    // transfer queued now would land on EP0.  Frames stay queued until then.
    if (!tud_mounted() || _usb.ep_in == 0u)
    {
        return;
    }

    // Atomically claim the IN endpoint and copy the oldest frame; the transfer
    // is started outside the critical section to keep interrupts disabled only
    // briefly.  The frame leaves the queue only once the transfer is running.
    uint32_t pm = cs_enter();
    if (_in_busy || _in_head == _in_tail)
    {
        cs_exit(pm);
        return;
    }
    _in_buf  = _in_queue[_in_tail];
    _in_busy = true;
    cs_exit(pm);

    bool ts = (_in_buf.channel < GS_USB_CAN_CHANNEL_COUNT) && _hw_timestamp[_in_buf.channel];
    uint16_t len;

    if (_in_buf.flags & GS_FRAME_FLAG_FD)
    {
        len = ts ? GS_HOST_FRAME_SIZE_FD_TS : GS_HOST_FRAME_SIZE_FD;
    }
    else
    {
        len = ts ? GS_HOST_FRAME_SIZE_TS : GS_HOST_FRAME_SIZE;
    }

    // gs_usb_report_frame() may run in an interrupt: tell TinyUSB which context
    // starts the transfer.
    if (usbd_edpt_xfer(_usb.rhport, _usb.ep_in, (uint8_t *)&_in_buf, len, __get_IPSR() != 0u))
    {
        // Only the _in_busy owner moves the tail, so no other context can pop
        // concurrently; the critical section keeps producers' view consistent.
        pm = cs_enter();
        _in_tail = (uint8_t)((_in_tail + 1u) % IN_QUEUE_SIZE);
        cs_exit(pm);
    }
    else
    {
        // Not started: keep the frame queued and release the endpoint so
        // gs_usb_task() retries, instead of leaving _in_busy stuck forever.
        _in_busy = false;
    }
}

// (Re)arm the OUT endpoint for the next host CAN TX frame, tracking success so
// gs_usb_task() can retry if the stack rejected the request.
static void out_arm(void)
{
    _out_armed = usbd_edpt_xfer(_usb.rhport, _usb.ep_out,
                                _out_buf.raw, sizeof(_out_buf.raw), false);
}

// Hand the host frame in _out_buf to the engine; false = not accepted yet.
static bool out_try_send(void)
{
    return gs_engine_send(_out_buf.frame.channel, &_out_buf.frame);
}

// CAN FD DLC code -> payload length in bytes
static uint8_t fd_dlc_to_len(uint8_t dlc)
{
    static const uint8_t len[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return len[dlc & 0x0Fu];
}

// Feature bits and nominal bit-timing constants, shared by BREQ_BT_CONST and
// BREQ_BT_CONST_EXT.
static void bt_const_fill(gs_device_bt_const_t *bt)
{
    // What the engine can actually do is a property of the engine, so the
    // advertised set comes from gs_usb_config.h.
    bt->feature   = GS_USB_FEATURES;
    bt->fclk_can  = gs_engine_can_clock_hz();
    bt->tseg1_min = GS_USB_TSEG1_MIN;
    bt->tseg1_max = GS_USB_TSEG1_MAX;
    bt->tseg2_min = GS_USB_TSEG2_MIN;
    bt->tseg2_max = GS_USB_TSEG2_MAX;
    bt->sjw_max   = GS_USB_SJW_MAX;
    bt->brp_min   = GS_USB_BRP_MIN;
    bt->brp_max   = GS_USB_BRP_MAX;
    bt->brp_inc   = GS_USB_BRP_INC;
}

//--------------------------------------------------------------------+
// TinyUSB class driver callbacks
//--------------------------------------------------------------------+

static void reset_usb_state(void)
{
    _in_busy    = false;
    _in_head    = 0;
    _in_tail    = 0;
    _out_armed  = false;
    _out_pending = false;
    memset(_hw_timestamp, 0, sizeof(_hw_timestamp));
    _usb.ep_in  = 0;
    _usb.ep_out = 0;
}

static void gsusb_init(void)
{
    // The CAN engine is initialised by the application; nothing to do here.
}

static bool gsusb_deinit(void)
{
    reset_usb_state();
    return true;
}

static void gsusb_reset(uint8_t rhport)
{
    (void)rhport;
    reset_usb_state();
    // The host session ended without MODE RESET; stop channels in the engine.
    gs_engine_reset();
}

static uint16_t gsusb_open(uint8_t rhport,
                            tusb_desc_interface_t const *desc_intf,
                            uint16_t max_len)
{
    (void)max_len;

    TU_VERIFY(desc_intf->bInterfaceClass    == 0xFFu, 0);
    TU_VERIFY(desc_intf->bInterfaceSubClass == 0xFFu, 0);
    TU_VERIFY(desc_intf->bInterfaceProtocol == 0xFFu, 0);

    _usb.itf_num = desc_intf->bInterfaceNumber;
    _usb.rhport  = rhport;

    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const *p_desc = tu_desc_next(desc_intf);

    for (uint8_t i = 0; i < desc_intf->bNumEndpoints; i++)
    {
        TU_ASSERT(tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT, 0);
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_desc;
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);

        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
        {
            _usb.ep_in = ep->bEndpointAddress;
        }
        else
        {
            _usb.ep_out = ep->bEndpointAddress;
        }

        drv_len += tu_desc_len(p_desc);
        p_desc = tu_desc_next(p_desc);
    }

    // Queue the first OUT receive to be ready for host CAN TX frames
    out_arm();

    return drv_len;
}

// Called by the shared tud_vendor_control_xfer_cb dispatcher in
// usb_app_drivers.c.  Returns true if this driver claimed the request.
bool gsusb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *request)
{
    // Only handle vendor requests addressed to our interface
    if (request->bmRequestType_bit.type      != TUSB_REQ_TYPE_VENDOR)    return false;
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
    if (request->wIndex != _usb.itf_num)                                  return false;

    if (stage == CONTROL_STAGE_SETUP)
    {
        // wValue carries the channel index for channel-specific requests
        uint8_t ch = (uint8_t)(request->wValue & 0xFFu);

        switch (request->bRequest)
        {

        // OUT requests — receive data from host --------------------------------

        case GS_USB_BREQ_HOST_FORMAT:
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_host_config_t));

        case GS_USB_BREQ_BITTIMING:
        case GS_USB_BREQ_DATA_BITTIMING:
            if (ch >= GS_USB_CAN_CHANNEL_COUNT) return false;
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_device_bittiming_t));

        case GS_USB_BREQ_MODE:
            if (ch >= GS_USB_CAN_CHANNEL_COUNT) return false;
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_device_mode_t));

        case GS_USB_BREQ_IDENTIFY:
            if (ch >= GS_USB_CAN_CHANNEL_COUNT) return false;
            if (request->wLength == 0u)
            {
                // No data stage, so the DATA-stage handler below never runs.
                gs_engine_identify(ch);
                return tud_control_status(rhport, request);
            }
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, request, _ctrl_buf.raw,
                                    TU_MIN(request->wLength, sizeof(_ctrl_buf.raw)));

        case GS_USB_BREQ_SET_USER_ID:
        case GS_USB_BREQ_SET_TERMINATION:
            // Receive and discard — not implemented
            return tud_control_xfer(rhport, request, _ctrl_buf.raw,
                                    TU_MIN(request->wLength, sizeof(_ctrl_buf.raw)));

        // IN requests — send data to host --------------------------------------

        case GS_USB_BREQ_DEVICE_CONFIG:
            _ctrl_buf.device_config.reserved1  = 0;
            _ctrl_buf.device_config.reserved2  = 0;
            _ctrl_buf.device_config.reserved3  = 0;
            _ctrl_buf.device_config.icount     = GS_USB_CAN_CHANNEL_COUNT - 1u;
            _ctrl_buf.device_config.sw_version = 2;
            _ctrl_buf.device_config.hw_version = 1;
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_device_config_t));

        case GS_USB_BREQ_BT_CONST:
            if (ch >= GS_USB_CAN_CHANNEL_COUNT) return false;
            bt_const_fill(&_ctrl_buf.bt_const);
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_device_bt_const_t));

        case GS_USB_BREQ_BT_CONST_EXT:
            if (ch >= GS_USB_CAN_CHANNEL_COUNT) return false;
            // Linux aborts probing the device if this request fails while
            // GS_CAN_FEATURE_BT_CONST_EXT is advertised.
            bt_const_fill(&_ctrl_buf.bt_const_ext.nominal);
            _ctrl_buf.bt_const_ext.dtseg1_min = GS_USB_DTSEG1_MIN;
            _ctrl_buf.bt_const_ext.dtseg1_max = GS_USB_DTSEG1_MAX;
            _ctrl_buf.bt_const_ext.dtseg2_min = GS_USB_DTSEG2_MIN;
            _ctrl_buf.bt_const_ext.dtseg2_max = GS_USB_DTSEG2_MAX;
            _ctrl_buf.bt_const_ext.dsjw_max   = GS_USB_DSJW_MAX;
            _ctrl_buf.bt_const_ext.dbrp_min   = GS_USB_DBRP_MIN;
            _ctrl_buf.bt_const_ext.dbrp_max   = GS_USB_DBRP_MAX;
            _ctrl_buf.bt_const_ext.dbrp_inc   = GS_USB_DBRP_INC;
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_device_bt_const_ext_t));

        case GS_USB_BREQ_GET_STATE:
            if (ch >= GS_USB_CAN_CHANNEL_COUNT) return false;
            memset(&_ctrl_buf.state, 0, sizeof(_ctrl_buf.state));
            gs_engine_get_state(ch, &_ctrl_buf.state);
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(gs_device_state_t));

        case GS_USB_BREQ_TIMESTAMP:
            // gs_usb timestamps are microseconds, same clock as timestamp_us
            _ctrl_buf.timestamp = gs_engine_timestamp_us();
            return tud_control_xfer(rhport, request,
                                    &_ctrl_buf, sizeof(uint32_t));

        case GS_USB_BREQ_BERR:
        case GS_USB_BREQ_GET_USER_ID:
        case GS_USB_BREQ_GET_TERMINATION:
            // Not implemented — return zeros
            memset(_ctrl_buf.raw, 0,
                   TU_MIN(request->wLength, sizeof(_ctrl_buf.raw)));
            return tud_control_xfer(rhport, request, _ctrl_buf.raw,
                                    TU_MIN(request->wLength, sizeof(_ctrl_buf.raw)));

        default:
            return false;
        }
    }

    if (stage == CONTROL_STAGE_DATA)
    {
        // Re-validate the channel saved at SETUP: a host that aborts and retries
        // between stages could leave _ctrl_ch stale before this DATA stage runs.
        if (_ctrl_ch >= GS_USB_CAN_CHANNEL_COUNT)
        {
            return true;
        }

        // Relay data received from the host to the CAN engine.
        switch (request->bRequest)
        {
        case GS_USB_BREQ_BITTIMING:
            gs_engine_set_bittiming(_ctrl_ch, &_ctrl_buf.bittiming);
            break;

        case GS_USB_BREQ_DATA_BITTIMING:
            gs_engine_set_data_bittiming(_ctrl_ch, &_ctrl_buf.bittiming);
            break;

        case GS_USB_BREQ_MODE:
            // Frame length on the IN endpoint is a transport concern: switch
            // it here rather than in the engine.
            _hw_timestamp[_ctrl_ch] = (_ctrl_buf.mode.mode == GS_CAN_MODE_START) &&
                                      ((_ctrl_buf.mode.flags & GS_CAN_FLAG_HW_TIMESTAMP) != 0u);
            gs_engine_set_mode(_ctrl_ch, &_ctrl_buf.mode);
            break;

        case GS_USB_BREQ_IDENTIFY:
            gs_engine_identify(_ctrl_ch);
            break;

        default:
            break;
        }
    }

    // CONTROL_STAGE_ACK: nothing to do
    return true;
}

static bool gsusb_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                           xfer_result_t result, uint32_t xferred_bytes)
{
    (void)rhport;

    if (ep_addr == _usb.ep_out)
    {
        // Host sent a CAN frame to transmit — relay it to the engine, which
        // transmits it and echoes it back to the host via gs_usb_report_frame().
        // Accept any frame that carries its whole payload: classic frames need
        // the 20-byte layout, FD frames at least header + DLC length.  Trailing
        // bytes (padding to the FD size, candle_api's timestamp_us) are ignored.
        const gs_host_frame_t *frame = &_out_buf.frame;
        uint32_t min_len = (frame->flags & GS_FRAME_FLAG_FD)
                               ? GS_HOST_FRAME_HDR_SIZE + fd_dlc_to_len(frame->can_dlc)
                               : GS_HOST_FRAME_SIZE;

        if (result == XFER_RESULT_SUCCESS &&
            xferred_bytes >= min_len &&
            frame->channel < GS_USB_CAN_CHANNEL_COUNT &&
            !out_try_send())
        {
            // Not accepted yet (TX queue full): keep the frame in
            // _out_buf and leave the OUT endpoint un-armed, so the host is
            // NAKed instead of losing the frame and its echo.  gs_usb_task()
            // retries and re-arms.
            _out_pending = true;
            return true;
        }

        // Re-arm OUT endpoint for the next frame; out_arm() records failure so
        // gs_usb_task() can retry instead of silently going deaf.
        out_arm();
    }
    else if (ep_addr == _usb.ep_in)
    {
        _in_busy = false;
        in_try_send();
    }

    return true;
}

//--------------------------------------------------------------------+
// Driver table — registered via weak-symbol override
//--------------------------------------------------------------------+

// Non-static so usb_app_drivers.c can build the shared driver table.
const usbd_class_driver_t gs_usb_driver =
{
    .name            = "GS_USB",
    .init            = gsusb_init,
    .deinit          = gsusb_deinit,
    .reset           = gsusb_reset,
    .open            = gsusb_open,
    .control_xfer_cb = NULL,  /* vendor requests routed via tud_vendor_control_xfer_cb */
    .xfer_cb         = gsusb_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void gs_usb_init(void)
{
    memset(&_usb, 0, sizeof(_usb));
    _in_busy = false;
    _in_head = 0;
    _in_tail = 0;
}

void gs_usb_task(void)
{
    // Service the CAN engine (no-op until the real backend is integrated).
    gs_engine_task();

    // Only touch endpoints once the device is configured (tud_mounted): before
    // SET_CONFIGURATION the endpoint addresses are still 0 (EP0).
    if (tud_mounted())
    {
        // Retry a host frame the engine could not accept yet; the OUT
        // endpoint stays un-armed until it goes through.
        if (_out_pending)
        {
            if (out_try_send())
            {
                _out_pending = false;
                out_arm();
            }
        }
        // Recover the OUT endpoint if a previous re-arm was rejected.
        else if (!_out_armed)
        {
            out_arm();
        }
        // Pump any frames the engine reported toward the host.
        in_try_send();
    }
}

bool gs_usb_report_frame(const gs_host_frame_t *frame)
{
    // Stamp every frame (RX and TX echo) here, so both share one clock.
    gs_host_frame_t stamped = *frame;
    if (stamped.flags & GS_FRAME_FLAG_FD)
    {
        stamped.fd.timestamp_us = gs_engine_timestamp_us();
    }
    else
    {
        stamped.classic.timestamp_us = gs_engine_timestamp_us();
    }

    bool is_rx = (stamped.echo_id == GS_ECHO_ID_RX) && !(stamped.can_id & GS_CAN_ERR_FLAG);
    bool ok    = in_queue_push(&stamped, is_rx ? IN_QUEUE_RESERVED : 0u);
    in_try_send();
    return ok;
}

//--------------------------------------------------------------------+
// Weak no-op CAN engine hooks
//
// The real CAN backend (integrated elsewhere) provides non-weak overrides
// that own the FDCAN peripheral.  Until then these stubs let the USB layer
// build, enumerate and exchange control / bulk transfers with the host while
// doing nothing on the bus.
//--------------------------------------------------------------------+

/*
 * Apply bit-timing for channel `ch` (BREQ_BITTIMING).  *bt fields:
 *   prop_seg, phase_seg1, phase_seg2, sjw, brp.  Real impl programs the FDCAN
 *   nominal timing (NominalTimeSeg1 = prop_seg + phase_seg1, TimeSeg2 =
 *   phase_seg2, SyncJumpWidth = sjw, Prescaler = brp) for use on the next start.
 *   Does NOT start the controller.
 */
__attribute__((weak)) void gs_engine_set_bittiming(uint8_t ch,
                                       const gs_device_bittiming_t *bt)
{
    (void)ch; (void)bt;
}

/*
 * Apply CAN FD data-phase bit-timing for channel `ch` (BREQ_DATA_BITTIMING),
 * same fields as gs_engine_set_bittiming().  Used on the next MODE START with
 * GS_CAN_FLAG_FD.  Does NOT start the controller.
 */
__attribute__((weak)) void gs_engine_set_data_bittiming(uint8_t ch,
                                                        const gs_device_bittiming_t *bt)
{
    (void)ch; (void)bt;
}

/*
 * Start / stop channel `ch` and select its operating mode (BREQ_MODE).
 *   mode->mode  == GS_CAN_MODE_START : bring FDCAN up with the last bit-timing
 *                  and begin RX/TX; otherwise stop it.
 *   mode->flags &  GS_CAN_FLAG_LISTEN_ONLY / _LOOP_BACK / _ONE_SHOT : select
 *                  the corresponding controller mode.
 * Once running, every received frame MUST be handed to the host via
 * gs_usb_report_frame() (echo_id = GS_ECHO_ID_RX).
 */
__attribute__((weak)) void gs_engine_set_mode(uint8_t ch,
                                              const gs_device_mode_t *mode)
{
    (void)ch; (void)mode;
}

/*
 * Transmit one CAN frame on channel `ch` (bulk OUT).  *frame is a gs_host_frame_t
 * (can_id with EFF/RTR flags, can_dlc, data[]).  After the controller has
 * transmitted the frame, echo it back to the host via gs_usb_report_frame() with the same
 * frame->echo_id so the host can match the send.  Return false if the frame
 * cannot be accepted right now; it is then retried from gs_usb_task().
 */
__attribute__((weak)) bool gs_engine_send(uint8_t ch,
                                          const gs_host_frame_t *frame)
{
    (void)ch; (void)frame;
    return true;
}

/*
 * Report channel `ch` state and error counters (BREQ_GET_STATE):
 *   state->state = GS_CAN_STATE_*, rxerr / txerr = controller error counters.
 */
__attribute__((weak)) void gs_engine_get_state(uint8_t ch, gs_device_state_t *state)
{
    (void)ch;
    state->state = GS_CAN_STATE_STOPPED;
}

/*
 * Free-running 32-bit microsecond counter used for BREQ_TIMESTAMP and the
 * timestamp_us of every frame sent to the host (wraps after ~71 minutes, which
 * the host handles).  The default derives it from the 1 kHz HAL tick and the
 * SysTick down-counter; an engine with a better time base (e.g. a free-running
 * TIM, or the FDCAN timestamp counter) may override it.
 */
__attribute__((weak)) uint32_t gs_engine_timestamp_us(void)
{
    uint32_t ms;
    uint32_t val;
    bool     pending;

    // Sample the millisecond tick and the SysTick down-counter consistently:
    // retry if the SysTick ISR updated the tick in between.
    do
    {
        ms      = HAL_GetTick();
        val     = SysTick->VAL;
        pending = (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) != 0u;
    } while (ms != HAL_GetTick());

    // Called with SysTick masked (ISR / critical section): the counter may
    // have wrapped without the tick being incremented yet.
    if (pending && val > (SysTick->LOAD / 2u))
    {
        ms++;
    }

    const uint32_t load = SysTick->LOAD + 1u;
    return (ms * 1000u) + (((load - 1u - val) * 1000u) / load);
}

/*
 * FDCAN kernel clock in Hz. The default is the configured constant; the engine
 * overrides it with the clock the peripheral actually runs at.
 */
__attribute__((weak)) uint32_t gs_engine_can_clock_hz(void)
{
    return GS_USB_FDCAN_CLK_HZ;
}

/*
 * Visual identify for channel `ch` (BREQ_IDENTIFY): flash an LED or similar.
 * May be a no-op if there is no indicator.
 */
__attribute__((weak)) void gs_engine_identify(uint8_t ch)
{
    (void)ch;
}

/*
 * Periodic service, called every gs_usb_task() iteration (main loop).
 * If the engine is polled (no RX interrupt), drain received frames here and
 * call gs_usb_report_frame() for each.  If it runs off interrupts, leave empty.
 */
__attribute__((weak)) void gs_engine_task(void)
{
}

/*
 * USB bus reset: stop every channel the host had started.
 */
__attribute__((weak)) void gs_engine_reset(void)
{
}
