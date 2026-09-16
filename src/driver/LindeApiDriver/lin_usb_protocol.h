#pragma once

/*
 * Host-side mirror of the lin_usb wire protocol.
 * No STM32 HAL or TinyUSB headers — only stdint and stdbool.
 * Keep in sync with Core/Inc/lin_usb.h on the firmware side.
 */

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * USB identification
 * ------------------------------------------------------------------------- */
#define LIN_USB_VID  0x1d50u
#define LIN_USB_PID  0x606fu

/* bInterfaceProtocol that distinguishes the LIN interface from CAN (0xFF) */
#define LIN_USB_ITF_PROTOCOL  0x01u

/* -------------------------------------------------------------------------
 * Vendor request codes (bRequest)
 * ------------------------------------------------------------------------- */
typedef enum
{
    LIN_USB_BREQ_HOST_FORMAT    = 0,
    LIN_USB_BREQ_BAUDRATE       = 1,
    LIN_USB_BREQ_MODE           = 2,
    LIN_USB_BREQ_DEVICE_CONFIG  = 3,
    LIN_USB_BREQ_TIMESTAMP      = 4,
    LIN_USB_BREQ_IDENTIFY       = 5,
    LIN_USB_BREQ_FRAME_CONFIG   = 6,
    LIN_USB_BREQ_SCHEDULE       = 7,
    LIN_USB_BREQ_BUS_STATE      = 8,
    LIN_USB_BREQ_SLEEP_WAKEUP   = 9,
} lin_usb_breq_t;

/* -------------------------------------------------------------------------
 * Mode constants
 * ------------------------------------------------------------------------- */
#define LIN_USB_MODE_STOP         0u
#define LIN_USB_MODE_START        1u
#define LIN_USB_MODE_PAUSE        2u

#define LIN_USB_FLAG_MASTER       0x01u  /* act as LIN master (sends break+sync) */
#define LIN_USB_FLAG_LISTEN_ONLY  0x02u  /* monitor: never transmit, report all  */

/* -------------------------------------------------------------------------
 * Device feature flags
 * ------------------------------------------------------------------------- */
#define LIN_USB_FEATURE_SCHEDULING      0x0001u
#define LIN_USB_FEATURE_TIMESTAMP       0x0002u
#define LIN_USB_FEATURE_CUSTOM_BAUDRATE 0x0004u
#define LIN_USB_FEATURE_BUS_STATE       0x0008u
#define LIN_USB_FEATURE_LISTEN_ONLY     0x0010u

/* -------------------------------------------------------------------------
 * Frame flags
 * ------------------------------------------------------------------------- */
#define LIN_USB_FRAME_FLAG_ENHANCED_CS  0x01u
#define LIN_USB_FRAME_FLAG_SUBSCRIBER   0x02u
#define LIN_USB_FRAME_FLAG_ERROR        0x04u
#define LIN_USB_FRAME_FLAG_WAKEUP       0x08u  /* wakeup event (device->host only) */
#define LIN_USB_FRAME_FLAG_TX_UPDATE    LIN_USB_FRAME_FLAG_WAKEUP  /* historic name */
#define LIN_USB_FRAME_FLAG_SPORADIC     0x10u
#define LIN_USB_FRAME_FLAG_RESPONDED    0x20u
#define LIN_USB_FRAME_FLAG_VALID        0x40u
#define LIN_USB_FRAME_FLAG_SLEEP        0x80u

/* -------------------------------------------------------------------------
 * Supported-baudrate bitmask
 * ------------------------------------------------------------------------- */
#define LIN_USB_BAUD_1200   0x01u
#define LIN_USB_BAUD_2400   0x02u
#define LIN_USB_BAUD_4800   0x04u
#define LIN_USB_BAUD_9600   0x08u
#define LIN_USB_BAUD_10417  0x10u
#define LIN_USB_BAUD_19200  0x20u
#define LIN_USB_BAUD_20000  0x40u

/* -------------------------------------------------------------------------
 * Misc constants
 * ------------------------------------------------------------------------- */
#define LIN_USB_ECHO_ID_RX  0xFFFFFFFFu

/* echo_id of a device->host acknowledgement of a bulk-OUT "set frame data":
 * not a bus event, LIN_USB_FRAME_FLAG_ERROR = LIN ID not in the running
 * schedule, payload unchanged. */
#define LIN_USB_ECHO_ID_SET_DATA_ACK  0u

/* Fallback slots per schedule table, used only when the device reports 0 in
 * DEVICE_CONFIG (firmware older than the schedule_entries field). */
#define LIN_USB_MAX_SCHEDULE_ENTRIES  16u

#define LIN_USB_VERSION_1_3   0u
#define LIN_USB_VERSION_2_0   1u
#define LIN_USB_VERSION_2_1   2u
#define LIN_USB_VERSION_2_2   3u
#define LIN_USB_VERSION_2_2A  4u
#define LIN_USB_VERSION_1X    LIN_USB_VERSION_1_3
#define LIN_USB_VERSION_2X    LIN_USB_VERSION_2_1

/* -------------------------------------------------------------------------
 * Wire structures (packed, same layout as firmware)
 * -------------------------------------------------------------------------
 * Using #pragma pack for MSVC + GCC/Clang portability.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct
{
    uint32_t echo_id;
    uint32_t timestamp_ms;
    uint8_t  lin_id;
    uint8_t  channel;
    uint8_t  dlc;
    uint8_t  flags;
    uint8_t  data[8];
} lin_usb_host_frame_t;

typedef struct
{
    uint8_t  byte_order;
    uint8_t  reserved[3];
} lin_usb_host_config_t;

typedef struct
{
    uint8_t  schedule_tables;
    uint8_t  supported_baudrates;
    uint8_t  schedule_entries;   /* slots per schedule table (0 = older firmware) */
    uint8_t  icount;
    uint32_t sw_version;
    uint32_t hw_version;
    uint32_t features;
} lin_usb_device_config_t;

typedef struct
{
    uint32_t baudrate;
    uint8_t  lin_version;
    uint8_t  break_length;
    uint8_t  timebase_ms;
    uint8_t  slave_nad;
    uint16_t jitter_us;
    uint16_t diag_stmin_ms;
    uint16_t diag_p2min_ms;
    uint16_t diag_nas_ms;
    uint16_t diag_ncr_ms;
    uint8_t  flags;       /* LIN_USB_FLAG_MASTER etc. */
    uint8_t  reserved;
} lin_usb_bus_config_t;

typedef struct
{
    uint8_t  mode;
    uint8_t  table_id;     /* START mode: schedule table index   */
    uint8_t  entry_count;  /* START mode: number of valid entries */
    uint8_t  reserved;
} lin_usb_mode_t;

typedef struct
{
    uint8_t  lin_id;
    uint8_t  direction;
    uint8_t  dlc;
    uint8_t  flags;
    uint16_t period_ms;
    uint8_t  table_id;
    uint8_t  reserved;
    uint8_t  data[8];
} lin_usb_schedule_entry_t;

typedef struct
{
    uint8_t command;
    uint8_t reserved[3];
} lin_usb_sleep_wakeup_t;

typedef enum
{
    LIN_USB_BUS_STATE_OK       = 0,
    LIN_USB_BUS_STATE_BUS_OFF  = 1,
    LIN_USB_BUS_STATE_PASSIVE  = 2,  /* deprecated, use _STOPPED */
    LIN_USB_BUS_STATE_ERROR    = 3,
    LIN_USB_BUS_STATE_STOPPED  = 4,
    LIN_USB_BUS_STATE_SLEEPING = 5,
} lin_usb_bus_state_e;

typedef struct
{
    uint8_t  state;       /* lin_usb_bus_state_e                                 */
    uint8_t  reserved;
    uint16_t dropped;     /* frames dropped because the device IN queue was full */
} lin_usb_bus_state_t;

#pragma pack(pop)
