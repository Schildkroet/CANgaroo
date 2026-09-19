#pragma once

/*
 * Host-side mirror of the gs_usb (CAN) wire protocol.
 * No STM32 HAL or TinyUSB headers — only stdint and stdbool.
 * Keep in sync with Core/Inc/gs_usb.h on the firmware side.
 */

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * USB identification
 * Same VID/PID as the LIN interface; CAN is discriminated by
 * bInterfaceProtocol = 0xFF (vs 0x01 for LIN).
 * ------------------------------------------------------------------------- */
#define GS_USB_VID           0x1d50u
#define GS_USB_PID           0x606fu
#define GS_USB_ITF_PROTOCOL  0xFFu

/* -------------------------------------------------------------------------
 * Vendor request codes (bRequest)
 * ------------------------------------------------------------------------- */
typedef enum
{
    GS_USB_BREQ_HOST_FORMAT   = 0,  /* OUT: host byte-order handshake                */
    GS_USB_BREQ_BITTIMING     = 1,  /* OUT: nominal bit timing for one channel       */
    GS_USB_BREQ_MODE          = 2,  /* OUT: start / reset one channel                */
    GS_USB_BREQ_BERR          = 3,  /* IN:  bus error counter (returns zeros)        */
    GS_USB_BREQ_BT_CONST      = 4,  /* IN:  timing constraints and feature flags     */
    GS_USB_BREQ_DEVICE_CONFIG = 5,  /* IN:  device capabilities, channel count       */
    GS_USB_BREQ_TIMESTAMP     = 6,  /* IN:  device time in microseconds (uint32)     */
    GS_USB_BREQ_IDENTIFY      = 7,  /* OUT: flash LED (no-op on current firmware)    */
    GS_USB_BREQ_GET_USER_ID   = 8,  /* not implemented                               */
    GS_USB_BREQ_SET_USER_ID   = 9,  /* not implemented                               */
    GS_USB_BREQ_DATA_BITTIMING = 10, /* OUT: CAN FD data-phase bit timing            */
    GS_USB_BREQ_BT_CONST_EXT  = 11, /* IN:  BT_CONST + CAN FD data-phase limits      */
    GS_USB_BREQ_SET_TERMINATION = 12, /* not implemented                             */
    GS_USB_BREQ_GET_TERMINATION = 13, /* not implemented                             */
    GS_USB_BREQ_GET_STATE     = 14, /* IN:  channel state + error counters           */
} gs_usb_breq_t;

/* Request numbers follow the Linux kernel driver (drivers/net/can/usb/gs_usb.c);
 * the candle_api source comments disagree on 9..13. */

/* -------------------------------------------------------------------------
 * CAN mode constants (gs_device_mode_t.mode)
 * ------------------------------------------------------------------------- */
#define GS_CAN_MODE_RESET  0u
#define GS_CAN_MODE_START  1u

/* -------------------------------------------------------------------------
 * CAN mode flags (gs_device_mode_t.flags)
 * ------------------------------------------------------------------------- */
#define GS_CAN_FLAG_LISTEN_ONLY    (1u << 0)  /* bus monitoring, no TX       */
#define GS_CAN_FLAG_LOOP_BACK      (1u << 1)  /* external loopback           */
#define GS_CAN_FLAG_TRIPLE_SAMPLE  (1u << 2)  /* triple sampling (passive)   */
#define GS_CAN_FLAG_ONE_SHOT       (1u << 3)  /* disable auto-retransmission */
#define GS_CAN_FLAG_HW_TIMESTAMP   (1u << 4)  /* IN frames carry timestamp_us */
#define GS_CAN_FLAG_FD             (1u << 8)  /* CAN FD mode                 */
#define GS_CAN_FLAG_AUTO_RESTART   (1u << 31) /* vendor extension: recover from bus-off */

/* -------------------------------------------------------------------------
 * Channel state (gs_device_state_t.state, GS_USB_BREQ_GET_STATE)
 * ------------------------------------------------------------------------- */
#define GS_CAN_STATE_ERROR_ACTIVE   0u
#define GS_CAN_STATE_ERROR_WARNING  1u
#define GS_CAN_STATE_ERROR_PASSIVE  2u
#define GS_CAN_STATE_BUS_OFF        3u
#define GS_CAN_STATE_STOPPED        4u
#define GS_CAN_STATE_SLEEPING       5u

/* -------------------------------------------------------------------------
 * Feature flags reported in gs_device_bt_const_t.feature
 * ------------------------------------------------------------------------- */
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
/* Vendor extension (not upstream gs_usb): GS_CAN_FLAG_AUTO_RESTART honoured. */
#define GS_CAN_FEATURE_AUTO_RESTART   (1u << 31)

/* -------------------------------------------------------------------------
 * CAN ID flags (embedded in the 32-bit can_id field, SocketCAN compatible)
 * ------------------------------------------------------------------------- */
#define GS_CAN_EFF_FLAG  0x80000000u  /* extended frame (29-bit ID) */
#define GS_CAN_RTR_FLAG  0x40000000u  /* remote transmission request */
#define GS_CAN_ERR_FLAG  0x20000000u  /* error frame */
#define GS_CAN_EFF_MASK  0x1FFFFFFFu  /* 29-bit extended ID mask */
#define GS_CAN_SFF_MASK  0x000007FFu  /* 11-bit standard ID mask */

/* SocketCAN error classes carried in can_id of GS_CAN_ERR_FLAG frames
 * (linux/can/error.h); details are in data[]. */
#define GS_CAN_ERR_CRTL      0x00000004u  /* controller state, data[1]         */
#define GS_CAN_ERR_PROT      0x00000008u  /* protocol violation, data[2..3]    */
#define GS_CAN_ERR_ACK       0x00000020u  /* no ACK on transmission            */
#define GS_CAN_ERR_BUSOFF    0x00000040u
#define GS_CAN_ERR_BUSERROR  0x00000080u
#define GS_CAN_ERR_RESTARTED 0x00000100u
#define GS_CAN_ERR_CNT       0x00000200u  /* TX/RX error counters in data[6]/[7] */

/* -------------------------------------------------------------------------
 * Frame flags (gs_host_frame_t.flags)
 * ------------------------------------------------------------------------- */
#define GS_FRAME_FLAG_OVERFLOW  (1u << 0)
#define GS_FRAME_FLAG_FD        (1u << 1)  /* CAN FD frame: fd.data[], DLC code 0–15 */
#define GS_FRAME_FLAG_BRS       (1u << 2)  /* bit-rate switch                       */
#define GS_FRAME_FLAG_ESI       (1u << 3)  /* error-state indicator                 */

/* echo_id value marking a frame received from the bus (not a TX echo) */
#define GS_ECHO_ID_RX  0xFFFFFFFFu

/* -------------------------------------------------------------------------
 * Bulk frame sizes on the wire
 *
 * 12-byte header + data[8] (classic) or data[64] (FD), plus a 4-byte
 * timestamp_us trailer on IN frames once the channel was started with
 * GS_CAN_FLAG_HW_TIMESTAMP.  An FD frame spans two 64-byte bulk packets.
 * ------------------------------------------------------------------------- */
#define GS_HOST_FRAME_HDR_SIZE    12u
#define GS_HOST_FRAME_SIZE        20u   /* classic              */
#define GS_HOST_FRAME_SIZE_TS     24u   /* classic + timestamp  */
#define GS_HOST_FRAME_SIZE_FD     76u   /* CAN FD               */
#define GS_HOST_FRAME_SIZE_FD_TS  80u   /* CAN FD + timestamp   */

/* CAN FD DLC code (0–15) <-> payload length in bytes */
static inline uint8_t gs_dlc_to_len(uint8_t dlc)
{
    static const uint8_t len[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return len[dlc & 0x0Fu];
}

/* Smallest DLC code whose payload holds `len` bytes (len > 64 -> 15). */
static inline uint8_t gs_len_to_dlc(uint8_t len)
{
    uint8_t dlc = 0;
    while (dlc < 15u && gs_dlc_to_len(dlc) < len)
    {
        dlc++;
    }
    return dlc;
}

/* -------------------------------------------------------------------------
 * Wire structures (packed, same layout as firmware)
 * -------------------------------------------------------------------------
 * Using #pragma pack for MSVC + GCC/Clang portability.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

/* Control OUT — GS_USB_BREQ_HOST_FORMAT (host→device): byte-order handshake */
typedef struct
{
    uint32_t byte_order;   /* write 0x0000BEEFu; device ignores the value */
} gs_host_config_t;

/* Control IN — GS_USB_BREQ_DEVICE_CONFIG (device→host): capabilities */
typedef struct
{
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint8_t  icount;       /* number of CAN channels minus 1 */
    uint32_t sw_version;
    uint32_t hw_version;
} gs_device_config_t;

/* Control IN — GS_USB_BREQ_BT_CONST (device→host): timing constraints */
typedef struct
{
    uint32_t feature;      /* GS_CAN_FEATURE_* bitmask */
    uint32_t fclk_can;     /* CAN peripheral clock in Hz (96 000 000 on this reference board) */
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
} gs_device_bt_const_t;

/* Control IN — GS_USB_BREQ_BT_CONST_EXT (device→host): BT_CONST followed by
 * the CAN FD data-phase limits */
typedef struct
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

/* Control OUT — GS_USB_BREQ_BITTIMING / GS_USB_BREQ_DATA_BITTIMING
 * (host→device): nominal / CAN FD data-phase bit timing */
typedef struct
{
    uint32_t prop_seg;     /* propagation segment (TQ) */
    uint32_t phase_seg1;   /* phase segment 1 (TQ) */
    uint32_t phase_seg2;   /* phase segment 2 (TQ) */
    uint32_t sjw;          /* sync jump width (TQ) */
    uint32_t brp;          /* baud rate prescaler */
} gs_device_bittiming_t;

/* Control OUT — GS_USB_BREQ_MODE (host→device): start or stop one channel */
typedef struct
{
    uint32_t mode;         /* GS_CAN_MODE_RESET / GS_CAN_MODE_START */
    uint32_t flags;        /* GS_CAN_FLAG_* bitmask */
} gs_device_mode_t;

/* Control IN — GS_USB_BREQ_GET_STATE (device→host) */
typedef struct
{
    uint32_t state;        /* GS_CAN_STATE_* */
    uint32_t rxerr;        /* receive error counter  */
    uint32_t txerr;        /* transmit error counter */
} gs_device_state_t;

/* Bulk EP OUT (host→device): frame to transmit on the CAN bus.
 * Bulk EP IN  (device→host): echo of sent frame, received frame, or error frame.
 * Only the first GS_HOST_FRAME_SIZE* bytes (see above) travel on the wire. */
typedef struct
{
    uint32_t echo_id;      /* GS_ECHO_ID_RX for bus-received; host-set for TX echo */
    uint32_t can_id;       /* 29/11-bit ID + GS_CAN_EFF/RTR/ERR flags in upper bits */
    uint8_t  can_dlc;      /* classic: 0–8 bytes; FD: DLC code 0–15 */
    uint8_t  channel;      /* CAN channel index (0-based) */
    uint8_t  flags;        /* GS_FRAME_FLAG_* */
    uint8_t  reserved;
    union
    {
        struct
        {
            uint8_t  data[8];
            uint32_t timestamp_us;  /* IN only, with GS_CAN_FLAG_HW_TIMESTAMP */
        } classic;
        struct
        {
            uint8_t  data[64];
            uint32_t timestamp_us;  /* IN only, with GS_CAN_FLAG_HW_TIMESTAMP */
        } fd;
    };
} gs_host_frame_t;

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(gs_host_frame_t) == GS_HOST_FRAME_SIZE_FD_TS, "gs_host_frame_t layout");
static_assert(sizeof(gs_device_bt_const_ext_t) == 72, "gs_device_bt_const_ext_t layout");
#endif
