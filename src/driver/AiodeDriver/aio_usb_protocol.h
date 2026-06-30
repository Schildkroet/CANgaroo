#pragma once

/*
 * Host-side mirror of the aio_usb wire protocol.
 * No STM32 HAL or TinyUSB headers — only stdint and stdbool.
 * Keep in sync with Core/Inc/aio_usb.h on the firmware side.
 */

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * USB identification
 * ------------------------------------------------------------------------- */
#define AIO_USB_VID  0x1d50u
#define AIO_USB_PID  0x606fu

/* bInterfaceProtocol that distinguishes the AIO interface from CAN (0xFF)
 * and LIN (0x01). */
#define AIO_USB_ITF_PROTOCOL  0x02u

/* -------------------------------------------------------------------------
 * Channel counts — mirror Core/Inc/aio_usb_config.h
 * ------------------------------------------------------------------------- */
#define AIO_USB_ANALOG_COUNT  16u

/* -------------------------------------------------------------------------
 * Vendor request codes (bRequest)
 * ------------------------------------------------------------------------- */
typedef enum
{
    AIO_USB_BREQ_HOST_FORMAT    = 0,
    AIO_USB_BREQ_DEVICE_CONFIG  = 1,
    AIO_USB_BREQ_TIMESTAMP      = 2,
    AIO_USB_BREQ_IDENTIFY       = 3,
    AIO_USB_BREQ_IO_CONFIG      = 4,
    AIO_USB_BREQ_IO_SET         = 5,
    AIO_USB_BREQ_READ_STATUS    = 6,
} aio_usb_breq_t;

/* -------------------------------------------------------------------------
 * I/O direction modes
 * ------------------------------------------------------------------------- */
#define AIO_USB_IO_MODE_INPUT   0u
#define AIO_USB_IO_MODE_OUTPUT  1u

/* -------------------------------------------------------------------------
 * I/O configuration flags
 * ------------------------------------------------------------------------- */
#define AIO_USB_IO_FLAG_PULLUP     0x01u
#define AIO_USB_IO_FLAG_PULLDOWN   0x02u
#define AIO_USB_IO_FLAG_ACTIVE_LOW 0x04u

/* -------------------------------------------------------------------------
 * Device feature flags
 * ------------------------------------------------------------------------- */
#define AIO_USB_FEATURE_TIMESTAMP   0x0001u
#define AIO_USB_FEATURE_AUTO_REPORT 0x0002u
#define AIO_USB_FEATURE_ANALOG      0x0004u

/* -------------------------------------------------------------------------
 * Wire structures (packed, same layout as firmware)
 * -------------------------------------------------------------------------
 * Using #pragma pack for MSVC + GCC/Clang portability.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct
{
    uint32_t byte_order;
} aio_usb_host_config_t;

typedef struct
{
    uint8_t  io_count;
    uint8_t  analog_count;
    uint8_t  analog_resolution_bits;
    uint8_t  reserved;
    uint32_t io_input_capable_mask;
    uint32_t io_output_capable_mask;
    uint32_t sw_version;
    uint32_t hw_version;
    uint32_t features;
} aio_usb_caps_t;

typedef struct
{
    uint8_t  mode;
    uint8_t  default_state;
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t auto_report_ms;
} aio_usb_io_config_t;

typedef struct
{
    uint32_t mask;
    uint32_t values;
} aio_usb_io_set_t;

typedef struct
{
    uint32_t timestamp_ms;
    uint32_t io_states;
    uint32_t io_direction;
    uint16_t analog[AIO_USB_ANALOG_COUNT];
} aio_usb_report_t;

#pragma pack(pop)
