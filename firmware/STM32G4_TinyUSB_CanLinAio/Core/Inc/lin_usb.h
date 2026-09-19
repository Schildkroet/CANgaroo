#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "stm32g4xx_hal.h"
#include "tusb.h"
#include "lin_usb_config.h"

/* -------------------------------------------------------------------------
 * Vendor request codes (bRequest field of USB control transfers)
 * ------------------------------------------------------------------------- */
typedef enum
{
    LIN_USB_BREQ_HOST_FORMAT    = 0,  /* OUT: host byte-order handshake        */
    LIN_USB_BREQ_BAUDRATE       = 1,  /* OUT: LIN baud rate + channel config   */
    LIN_USB_BREQ_MODE           = 2,  /* OUT: start / stop channel and schedule */
    LIN_USB_BREQ_DEVICE_CONFIG  = 3,  /* IN:  capabilities, channel count      */
    LIN_USB_BREQ_TIMESTAMP      = 4,  /* IN:  HAL_GetTick() value              */
    LIN_USB_BREQ_IDENTIFY       = 5,  /* OUT: flash LED (optional, no-op ok)   */
    LIN_USB_BREQ_FRAME_CONFIG   = 6,  /* OUT: per-ID TX data update            */
    LIN_USB_BREQ_SCHEDULE       = 7,  /* OUT: upload one schedule entry        */
    LIN_USB_BREQ_BUS_STATE      = 8,  /* IN:  current bus state per channel    */
    LIN_USB_BREQ_SLEEP_WAKEUP   = 9,  /* OUT: go-to-sleep or wakeup pulse      */
} lin_usb_breq_t;

/* -------------------------------------------------------------------------
 * Mode constants (lin_usb_mode_t.mode)
 * ------------------------------------------------------------------------- */
#define LIN_USB_MODE_STOP         0u  /* stop channel and schedule              */
#define LIN_USB_MODE_START        1u  /* start channel; table_id / entry_count select the schedule table */
#define LIN_USB_MODE_PAUSE        2u  /* pause schedule at current slot         */

/* Bus config flags (lin_usb_bus_config_t.flags) */
#define LIN_USB_FLAG_MASTER       0x01u  /* act as LIN master (sends break+sync)   */
#define LIN_USB_FLAG_LISTEN_ONLY  0x02u  /* monitor: never transmit, report all    */

/* -------------------------------------------------------------------------
 * Device feature flags (lin_usb_device_config_t.features)
 * ------------------------------------------------------------------------- */
#define LIN_USB_FEATURE_SCHEDULING      0x0001u  /* device can run its own schedule   */
#define LIN_USB_FEATURE_TIMESTAMP       0x0002u  /* timestamps are valid              */
#define LIN_USB_FEATURE_CUSTOM_BAUDRATE 0x0004u  /* arbitrary baud rates accepted     */
#define LIN_USB_FEATURE_BUS_STATE       0x0008u  /* BREQ_BUS_STATE is supported       */
#define LIN_USB_FEATURE_LISTEN_ONLY     0x0010u  /* LIN_USB_FLAG_LISTEN_ONLY honoured */

/* -------------------------------------------------------------------------
 * Bulk frame flags (lin_usb_host_frame_t.flags)
 * ------------------------------------------------------------------------- */
#define LIN_USB_FRAME_FLAG_ENHANCED_CS  0x01u  /* LIN 2.x enhanced checksum          */
#define LIN_USB_FRAME_FLAG_SUBSCRIBER   0x02u  /* master header only, slave replies   */
#define LIN_USB_FRAME_FLAG_ERROR        0x04u  /* frame carries error information     */
#define LIN_USB_FRAME_FLAG_WAKEUP       0x08u  /* wakeup event (device→host only)     */
/* Historic name of the same bit: a bulk-OUT "set frame" always updates schedule
 * data, so the host→device meaning was never needed. */
#define LIN_USB_FRAME_FLAG_TX_UPDATE    LIN_USB_FRAME_FLAG_WAKEUP
#define LIN_USB_FRAME_FLAG_SPORADIC     0x10u  /* TX only when data has been updated  */
#define LIN_USB_FRAME_FLAG_RESPONDED    0x20u  /* slave responded (device→host only)  */
#define LIN_USB_FRAME_FLAG_VALID        0x40u  /* checksum matched (device→host only) */
#define LIN_USB_FRAME_FLAG_SLEEP        0x80u  /* go-to-sleep event (device→host only) */

/* Supported-baudrate bitmask (lin_usb_device_config_t.supported_baudrates) */
#define LIN_USB_BAUD_1200   0x01u
#define LIN_USB_BAUD_2400   0x02u
#define LIN_USB_BAUD_4800   0x04u
#define LIN_USB_BAUD_9600   0x08u
#define LIN_USB_BAUD_10417  0x10u  /* 1 MHz / 96, common LIN standard rate */
#define LIN_USB_BAUD_19200  0x20u
#define LIN_USB_BAUD_20000  0x40u

/* echo_id value that marks a bus event (received frame or transmitted frame) */
#define LIN_USB_ECHO_ID_RX  0xFFFFFFFFu

/* echo_id of a device→host acknowledgement of a bulk-OUT "set frame data".
 * Not a bus event: it only says whether the LIN ID was found in the running
 * schedule (LIN_USB_FRAME_FLAG_ERROR = not found, payload unchanged). */
#define LIN_USB_ECHO_ID_SET_DATA_ACK  0u

/* LIN version codes used in lin_usb_bus_config_t.lin_version */
#define LIN_USB_VERSION_1_3   0u  /* classic checksum (data bytes only)   */
#define LIN_USB_VERSION_2_0   1u  /* enhanced checksum (PID + data bytes) */
#define LIN_USB_VERSION_2_1   2u
#define LIN_USB_VERSION_2_2   3u
#define LIN_USB_VERSION_2_2A  4u
#define LIN_USB_VERSION_1X    LIN_USB_VERSION_1_3  /* backward-compat alias */
#define LIN_USB_VERSION_2X    LIN_USB_VERSION_2_1  /* backward-compat alias */

/* -------------------------------------------------------------------------
 * USB data structures (all packed to avoid padding)
 * ------------------------------------------------------------------------- */

/* Bulk EP OUT (host→device): "set frame data" — update the payload the matching
 *   publisher slot sends on its next schedule event. Nothing goes out
 *   immediately: the device is self-scheduling.
 * Bulk EP IN  (device→host): a bus event (echo_id == LIN_USB_ECHO_ID_RX,
 *   transmitted or received frame), or the acknowledgement of a "set frame
 *   data" (echo_id == LIN_USB_ECHO_ID_SET_DATA_ACK). */
typedef struct TU_ATTR_PACKED
{
    uint32_t echo_id;       /* LIN_USB_ECHO_ID_RX for bus-received frames     */
    uint32_t timestamp_ms;  /* HAL_GetTick() at frame event                  */
    uint8_t  lin_id;        /* Protected LIN ID (6-bit ID + P0/P1 parity)    */
    uint8_t  channel;       /* Channel index (0-based)                       */
    uint8_t  dlc;           /* Data length: 1-8                              */
    uint8_t  flags;         /* LIN_USB_FRAME_FLAG_*                          */
    uint8_t  data[8];       /* Frame payload                                 */
} lin_usb_host_frame_t;

/* Control OUT — LIN_USB_BREQ_HOST_FORMAT (host→device): byte-order handshake */
typedef struct TU_ATTR_PACKED
{
    uint8_t  byte_order;
    uint8_t  reserved[3];
} lin_usb_host_config_t;

/* Control IN — LIN_USB_BREQ_DEVICE_CONFIG (device→host): capabilities */
typedef struct TU_ATTR_PACKED
{
    uint8_t  schedule_tables;     /* schedule tables available per channel    */
    uint8_t  supported_baudrates; /* LIN_USB_BAUD_* bitmask                   */
    uint8_t  schedule_entries;    /* slots per schedule table                 */
    uint8_t  icount;              /* number of channels - 1                   */
    uint32_t sw_version;
    uint32_t hw_version;
    uint32_t features;            /* LIN_USB_FEATURE_* bitmask                */
} lin_usb_device_config_t;

/* Control OUT — LIN_USB_BREQ_BAUDRATE (host→device): baud rate + channel config */
typedef struct TU_ATTR_PACKED
{
    uint32_t baudrate;       /* LIN baud rate in bps, e.g. 19200                  */
    uint8_t  lin_version;    /* LIN_USB_VERSION_*                                 */
    uint8_t  break_length;   /* break field length in bit times (default 13)      */
    uint8_t  timebase_ms;    /* nominal schedule slot floor in ms (0 = unused)    */
    uint8_t  slave_nad;      /* diagnostic NAD filter (0x7F = broadcast)          */
    uint16_t jitter_us;      /* additional inter-frame jitter in µs               */
    uint16_t diag_stmin_ms;  /* min separation between consecutive CF frames      */
    uint16_t diag_p2min_ms;  /* min delay before scheduling the 0x3D response     */
    uint16_t diag_nas_ms;    /* TX frame abort timeout (ms)                       */
    uint16_t diag_ncr_ms;    /* RX response wait timeout (ms)                     */
    uint8_t  flags;          /* LIN_USB_FLAG_MASTER etc.                          */
    uint8_t  reserved;
} lin_usb_bus_config_t;

/* Control OUT — LIN_USB_BREQ_MODE (host→device): channel and schedule control.
 * table_id / entry_count select the schedule table when mode == LIN_USB_MODE_START. */
typedef struct TU_ATTR_PACKED
{
    uint8_t  mode;         /* LIN_USB_MODE_*                            */
    uint8_t  table_id;     /* START mode: schedule table index          */
    uint8_t  entry_count;  /* START mode: number of valid entries        */
    uint8_t  reserved;
} lin_usb_mode_t;

/* Control OUT — LIN_USB_BREQ_SCHEDULE / BREQ_FRAME_CONFIG (host→device):
 * one slot in a schedule table (16 bytes).
 * table_id selects which of the LIN_USB_MAX_SCHEDULE_TABLES tables this
 * entry belongs to; wValue[15:8] carries the slot index within that table. */
typedef struct TU_ATTR_PACKED
{
    uint8_t  lin_id;       /* Protected LIN ID                              */
    uint8_t  direction;    /* 0 = publisher (TX), 1 = subscriber (RX resp.) */
    uint8_t  dlc;          /* Data length: 1-8                              */
    uint8_t  flags;        /* LIN_USB_FRAME_FLAG_*                          */
    uint16_t period_ms;    /* Slot period in milliseconds                   */
    uint8_t  table_id;     /* Schedule table index (0 .. MAX_TABLES-1)      */
    uint8_t  reserved;
    uint8_t  data[8];      /* TX payload for publisher frames               */
} lin_usb_schedule_entry_t;

/* Control IN — LIN_USB_BREQ_BUS_STATE (device→host): per-channel bus state */
typedef enum
{
    LIN_USB_BUS_STATE_OK       = 0,  /* nominal operation                          */
    LIN_USB_BUS_STATE_BUS_OFF  = 1,  /* fatal errors; bus unusable                 */
    LIN_USB_BUS_STATE_PASSIVE  = 2,  /* deprecated, use _STOPPED                   */
    LIN_USB_BUS_STATE_ERROR    = 3,  /* recoverable errors (no response, checksum) */
    LIN_USB_BUS_STATE_STOPPED  = 4,  /* channel not started by the host            */
    LIN_USB_BUS_STATE_SLEEPING = 5,  /* started, but the bus is in sleep state     */
} lin_usb_bus_state_e;

typedef struct TU_ATTR_PACKED
{
    uint8_t  state;       /* lin_usb_bus_state_e                                  */
    uint8_t  reserved;
    uint16_t dropped;     /* frames dropped because the device IN queue was full  */
} lin_usb_bus_state_t;

/* Control OUT — LIN_USB_BREQ_SLEEP_WAKEUP (host→device) */
typedef struct TU_ATTR_PACKED
{
    uint8_t command;      /* 0 = go-to-sleep, 1 = wakeup */
    uint8_t reserved[3];
} lin_usb_sleep_wakeup_t;

/* -------------------------------------------------------------------------
 * Public API
 *
 * This driver is *only* the USB transport between the host and the LIN engine.
 * It does no LIN scheduling, timing or bus access itself — those live in a
 * separate LIN scheduler (integrated elsewhere) that overrides the weak
 * lin_engine_* hooks below.
 * ------------------------------------------------------------------------- */

/* Call once after tusb_init(). */
void lin_usb_init(void);

/* Call in the main loop alongside tud_task(): services the engine hook and
 * pumps queued frames toward the host. */
void lin_usb_task(void);

/* Internal: vendor control handler called by the dispatcher in usb_app_drivers.c. */
bool linusb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                             tusb_control_request_t const *req);

/*
 * Engine -> host: queue a processed LIN frame (a transmitted publish or a
 * received frame) to be streamed to the host over the bulk IN endpoint.
 * The LIN engine calls this whenever it processes a frame.
 * Returns false if the queue is full.
 */
bool lin_usb_report_frame(const lin_usb_host_frame_t *frame);

/* -------------------------------------------------------------------------
 * LIN engine hooks
 *
 * Host requests are relayed 1:1 to these hooks.  Weak no-op defaults live in
 * lin_usb.c so this driver builds and enumerates stand-alone; the real LIN
 * scheduler provides non-weak overrides that own the schedule table, timing,
 * master/slave behaviour and the LIN bus (UART).
 * ------------------------------------------------------------------------- */

/* Apply baud rate / timing configuration for a channel. */
void lin_engine_configure(uint8_t ch, const lin_usb_bus_config_t *cfg);

/* Start / stop a channel; role (master/slave) is taken from lin_usb_bus_config_t. */
void lin_engine_set_mode(uint8_t ch, const lin_usb_mode_t *mode);

/* Install one schedule-table entry at the given slot. */
void lin_engine_set_schedule_entry(uint8_t ch, uint8_t slot,
                                   const lin_usb_schedule_entry_t *entry);

/* Update an existing entry's parameters (matched by LIN ID). */
void lin_engine_update_frame(uint8_t ch, const lin_usb_schedule_entry_t *entry);

/* Go-to-sleep (command 0) or wakeup (command 1). */
void lin_engine_sleep_wakeup(uint8_t ch, uint8_t command);

/* "Set frame data": set the payload the matching publisher entry sends on its
 * next schedule event. Returns false if no publisher slot in the active table
 * carries this LIN ID, in which case nothing was updated. */
bool lin_engine_set_data(uint8_t ch, const lin_usb_host_frame_t *frame);

/* Visual identify (flash an LED, etc.). */
void lin_engine_identify(uint8_t ch);

/* Query the current bus state for a channel (ok / bus_off / passive / error). */
void lin_engine_get_bus_state(uint8_t ch, lin_usb_bus_state_t *state);

/* Periodic service hook, called from lin_usb_task(). */
void lin_engine_task(void);

/* USB bus reset: stop every channel (called from tud_task(), main-loop context). */
void lin_engine_reset(void);
