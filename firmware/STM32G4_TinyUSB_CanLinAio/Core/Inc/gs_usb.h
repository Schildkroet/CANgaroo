/*
 * gs_usb.h — USB CAN adapter driver (Candlelight/gs_usb protocol)
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
 * Compatible with the Linux kernel gs_usb driver
 * (drivers/net/can/usb/gs_usb.c).
 */

#ifndef GS_USB_H_
#define GS_USB_H_

#include "tusb.h"
#include "stm32g4xx_hal.h"
#include "gs_usb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// gs_usb vendor request codes
//--------------------------------------------------------------------+
#define GS_USB_BREQ_HOST_FORMAT       0u
#define GS_USB_BREQ_BITTIMING         1u
#define GS_USB_BREQ_MODE              2u
#define GS_USB_BREQ_BERR              3u
#define GS_USB_BREQ_BT_CONST          4u
#define GS_USB_BREQ_DEVICE_CONFIG     5u
#define GS_USB_BREQ_TIMESTAMP         6u
#define GS_USB_BREQ_IDENTIFY          7u
#define GS_USB_BREQ_GET_USER_ID       8u
#define GS_USB_BREQ_SET_USER_ID       9u
#define GS_USB_BREQ_DATA_BITTIMING    10u
#define GS_USB_BREQ_BT_CONST_EXT      11u
#define GS_USB_BREQ_SET_TERMINATION   12u
#define GS_USB_BREQ_GET_TERMINATION   13u
#define GS_USB_BREQ_GET_STATE         14u

//--------------------------------------------------------------------+
// Device mode (gs_device_mode.mode field)
//--------------------------------------------------------------------+
#define GS_CAN_MODE_RESET             0u
#define GS_CAN_MODE_START             1u

//--------------------------------------------------------------------+
// Channel state (gs_device_state.state field, BREQ_GET_STATE)
//--------------------------------------------------------------------+
#define GS_CAN_STATE_ERROR_ACTIVE     0u
#define GS_CAN_STATE_ERROR_WARNING    1u
#define GS_CAN_STATE_ERROR_PASSIVE    2u
#define GS_CAN_STATE_BUS_OFF          3u
#define GS_CAN_STATE_STOPPED          4u
#define GS_CAN_STATE_SLEEPING         5u

//--------------------------------------------------------------------+
// Mode flags (gs_device_mode.flags field)
//--------------------------------------------------------------------+
#define GS_CAN_FLAG_LISTEN_ONLY       (1u << 0)
#define GS_CAN_FLAG_LOOP_BACK         (1u << 1)
#define GS_CAN_FLAG_TRIPLE_SAMPLE     (1u << 2)
#define GS_CAN_FLAG_ONE_SHOT          (1u << 3)
#define GS_CAN_FLAG_HW_TIMESTAMP      (1u << 4)
#define GS_CAN_FLAG_FD                (1u << 8)
#define GS_CAN_FLAG_AUTO_RESTART      (1u << 31)   // vendor extension, see GS_CAN_FEATURE_AUTO_RESTART

//--------------------------------------------------------------------+
// Feature flags (reported in gs_device_bt_const.feature)
//--------------------------------------------------------------------+
#define GS_CAN_FEATURE_LISTEN_ONLY    (1u << 0)
#define GS_CAN_FEATURE_LOOP_BACK      (1u << 1)
#define GS_CAN_FEATURE_TRIPLE_SAMPLE  (1u << 2)
#define GS_CAN_FEATURE_ONE_SHOT       (1u << 3)
#define GS_CAN_FEATURE_HW_TIMESTAMP   (1u << 4)
#define GS_CAN_FEATURE_IDENTIFY       (1u << 5)
#define GS_CAN_FEATURE_USER_ID        (1u << 6)
#define GS_CAN_FEATURE_FD             (1u << 8)
#define GS_CAN_FEATURE_BT_CONST_EXT   (1u << 10)
#define GS_CAN_FEATURE_GET_STATE      (1u << 13)

// Vendor extension, not part of upstream gs_usb: the channel recovers from
// bus-off by itself after a fixed delay, enabled per channel with
// GS_CAN_FLAG_AUTO_RESTART.  Bit 31 is far above the highest upstream bit, so
// the Linux driver and stock candle_api never set or interpret it.
#define GS_CAN_FEATURE_AUTO_RESTART   (1u << 31)

//--------------------------------------------------------------------+
// CAN ID flags (socket CAN compatible, in gs_host_frame.can_id)
//--------------------------------------------------------------------+
#define GS_CAN_EFF_FLAG               0x80000000U
#define GS_CAN_RTR_FLAG               0x40000000U
#define GS_CAN_ERR_FLAG               0x20000000U
#define GS_CAN_EFF_MASK               0x1FFFFFFFU
#define GS_CAN_SFF_MASK               0x000007FFU

// SocketCAN error classes carried in can_id of GS_CAN_ERR_FLAG frames
// (linux/can/error.h)
#define GS_CAN_ERR_DLC                8u
#define GS_CAN_ERR_CRTL               0x00000004U   // controller state, data[1]
#define GS_CAN_ERR_PROT               0x00000008U   // protocol violation, data[2..3]
#define GS_CAN_ERR_ACK                0x00000020U   // no ACK on transmission
#define GS_CAN_ERR_BUSOFF             0x00000040U
#define GS_CAN_ERR_BUSERROR           0x00000080U
#define GS_CAN_ERR_RESTARTED          0x00000100U
#define GS_CAN_ERR_CNT                0x00000200U   // TX/RX error counters in data[6]/data[7]

// data[1] of GS_CAN_ERR_CRTL frames
#define GS_CAN_ERR_CRTL_RX_WARNING    0x04u
#define GS_CAN_ERR_CRTL_TX_WARNING    0x08u
#define GS_CAN_ERR_CRTL_RX_PASSIVE    0x10u
#define GS_CAN_ERR_CRTL_TX_PASSIVE    0x20u
#define GS_CAN_ERR_CRTL_ACTIVE        0x40u

// data[2] (type) and data[3] (location) of GS_CAN_ERR_PROT frames
#define GS_CAN_ERR_PROT_FORM          0x02u
#define GS_CAN_ERR_PROT_STUFF         0x04u
#define GS_CAN_ERR_PROT_BIT0          0x08u
#define GS_CAN_ERR_PROT_BIT1          0x10u
#define GS_CAN_ERR_PROT_LOC_CRC_SEQ   0x08u
#define GS_CAN_ERR_PROT_LOC_ACK       0x19u

//--------------------------------------------------------------------+
// Frame flags (gs_host_frame.flags field)
//--------------------------------------------------------------------+
#define GS_FRAME_FLAG_OVERFLOW        (1u << 0)
#define GS_FRAME_FLAG_FD              (1u << 1)
#define GS_FRAME_FLAG_BRS             (1u << 2)
#define GS_FRAME_FLAG_ESI             (1u << 3)

// echo_id value for frames received from the CAN bus
#define GS_ECHO_ID_RX                 0xFFFFFFFFU

//--------------------------------------------------------------------+
// gs_usb protocol structures (little-endian, packed)
//--------------------------------------------------------------------+

typedef struct TU_ATTR_PACKED
{
    uint32_t byte_order;
} gs_host_config_t;

typedef struct TU_ATTR_PACKED
{
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint8_t  icount;        // number of CAN channels minus 1
    uint32_t sw_version;
    uint32_t hw_version;
} gs_device_config_t;

typedef struct TU_ATTR_PACKED
{
    uint32_t feature;
    uint32_t fclk_can;      // CAN peripheral kernel clock in Hz
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
} gs_device_bt_const_t;

// BREQ_BT_CONST_EXT: the BREQ_BT_CONST fields followed by the CAN FD data phase
typedef struct TU_ATTR_PACKED
{
    gs_device_bt_const_t nominal;
    uint32_t dtseg1_min;
    uint32_t dtseg1_max;
    uint32_t dtseg2_min;
    uint32_t dtseg2_max;
    uint32_t dsjw_max;
    uint32_t dbrp_min;
    uint32_t dbrp_max;
    uint32_t dbrp_inc;
} gs_device_bt_const_ext_t;

typedef struct TU_ATTR_PACKED
{
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
} gs_device_bittiming_t;

typedef struct TU_ATTR_PACKED
{
    uint32_t mode;
    uint32_t flags;
} gs_device_mode_t;

// BREQ_GET_STATE reply
typedef struct TU_ATTR_PACKED
{
    uint32_t state;         // GS_CAN_STATE_*
    uint32_t rxerr;         // receive error counter
    uint32_t txerr;         // transmit error counter
} gs_device_state_t;

// Frame exchanged on the bulk endpoints: a 12-byte header followed by the
// classic (data[8]) or, with GS_FRAME_FLAG_FD, the CAN FD (data[64]) layout.
// The 4-byte timestamp_us trailer is only on the wire once the host started
// the channel with GS_CAN_FLAG_HW_TIMESTAMP (Linux does whenever the feature
// is advertised, Windows candle_api always does).
#define GS_HOST_FRAME_HDR_SIZE      12u
#define GS_HOST_FRAME_SIZE          20u     // classic
#define GS_HOST_FRAME_SIZE_TS       24u     // classic + timestamp
#define GS_HOST_FRAME_SIZE_FD       76u     // CAN FD
#define GS_HOST_FRAME_SIZE_FD_TS    80u     // CAN FD + timestamp

typedef struct TU_ATTR_PACKED
{
    uint32_t echo_id;       // GS_ECHO_ID_RX for bus-received frames
    uint32_t can_id;        // ID with EFF/RTR/ERR flag bits
    uint8_t  can_dlc;       // classic: 0–8 bytes; FD: DLC code 0–15
    uint8_t  channel;       // CAN channel index
    uint8_t  flags;         // GS_FRAME_FLAG_*
    uint8_t  reserved;
    union TU_ATTR_PACKED
    {
        struct TU_ATTR_PACKED
        {
            uint8_t  data[8];
            uint32_t timestamp_us;  // filled in by gs_usb_report_frame()
        } classic;
        struct TU_ATTR_PACKED
        {
            uint8_t  data[64];
            uint32_t timestamp_us;  // filled in by gs_usb_report_frame()
        } fd;
    };
} gs_host_frame_t;

//--------------------------------------------------------------------+
// Public API
//
// This driver is *only* the USB transport between the host and the CAN engine.
// It does no FDCAN access itself — bit-timing, mode, TX and RX live in a
// separate CAN backend (integrated elsewhere) that overrides the weak
// gs_engine_* hooks below.
//--------------------------------------------------------------------+

// Call once after tusb_init(), before the main loop.
void gs_usb_init(void);

// Call every iteration of the main loop (alongside tud_task()): services the
// engine hook and pumps queued frames toward the host.
void gs_usb_task(void);

// Internal: vendor control handler called by the dispatcher in usb_app_drivers.c.
bool gsusb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *request);

// Engine -> host: queue a frame (received frame or TX echo) to be streamed to
// the host over bulk IN.  The CAN engine calls this for every processed frame.
// Returns false if the queue is full.
bool gs_usb_report_frame(const gs_host_frame_t *frame);

//--------------------------------------------------------------------+
// CAN engine hooks
//
// Host requests are relayed 1:1 to these hooks.  Weak no-op defaults live in
// gs_usb.c so this driver builds and enumerates stand-alone; the real CAN
// backend provides non-weak overrides that own the FDCAN peripheral (timing,
// mode, TX) and report received frames / TX echoes via gs_usb_report_frame().
//--------------------------------------------------------------------+

// Apply bit-timing configuration for a channel (BREQ_BITTIMING).
void gs_engine_set_bittiming(uint8_t ch, const gs_device_bittiming_t *bt);

// Apply CAN FD data-phase bit-timing for a channel (BREQ_DATA_BITTIMING).
void gs_engine_set_data_bittiming(uint8_t ch, const gs_device_bittiming_t *bt);

// Start / stop a channel and select its mode (BREQ_MODE).
void gs_engine_set_mode(uint8_t ch, const gs_device_mode_t *mode);

// Transmit one CAN frame (bulk OUT) and echo it back via gs_usb_report_frame()
// once it has actually been transmitted on the bus.
// Return false if the frame cannot be accepted right now (TX queue full): the
// transport then keeps it and stops reading the OUT endpoint until a retry
// succeeds, instead of dropping a frame whose echo the host is waiting for.
bool gs_engine_send(uint8_t ch, const gs_host_frame_t *frame);

// Visual identify (flash an LED, etc.).
void gs_engine_identify(uint8_t ch);

// Current channel state and error counters (BREQ_GET_STATE).
void gs_engine_get_state(uint8_t ch, gs_device_state_t *state);

// FDCAN kernel clock in Hz, reported to the host in BREQ_BT_CONST so it can
// compute bit timing. The engine reads the configured clock tree.
uint32_t gs_engine_can_clock_hz(void);

// Periodic service hook, called from gs_usb_task().
void gs_engine_task(void);

// USB bus reset: stop every channel (called from tud_task(), main-loop context).
void gs_engine_reset(void);

// Free-running microsecond counter for BREQ_TIMESTAMP and frame timestamp_us.
// The weak default derives it from HAL_GetTick() + SysTick.
uint32_t gs_engine_timestamp_us(void);

// Note for engine implementers: only report frames with GS_FRAME_FLAG_FD while
// the host started that channel with GS_CAN_FLAG_FD (see gs_engine_set_mode());
// a classic-mode host cannot parse the 76/80-byte FD layout.

#ifdef __cplusplus
}
#endif

#endif /* GS_USB_H_ */
