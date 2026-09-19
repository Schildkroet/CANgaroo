#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "stm32g4xx_hal.h"
#include "tusb.h"
#include "aio_usb_config.h"

/* -------------------------------------------------------------------------
 * Vendor request codes (bRequest field of USB control transfers)
 * ------------------------------------------------------------------------- */
typedef enum
{
    AIO_USB_BREQ_HOST_FORMAT    = 0,  /* OUT: host byte-order handshake        */
    AIO_USB_BREQ_DEVICE_CONFIG  = 1,  /* IN:  capabilities                     */
    AIO_USB_BREQ_TIMESTAMP      = 2,  /* IN:  HAL_GetTick() value              */
    AIO_USB_BREQ_IDENTIFY       = 3,  /* OUT: flash LED (optional, no-op ok)   */
    AIO_USB_BREQ_IO_CONFIG      = 4,  /* OUT: configure one I/O line           */
    AIO_USB_BREQ_IO_SET         = 5,  /* OUT: set output line states           */
    AIO_USB_BREQ_READ_STATUS    = 6,  /* IN:  full I/O + analog snapshot        */
} aio_usb_breq_t;

/* -------------------------------------------------------------------------
 * I/O direction modes (aio_usb_io_config_t.mode)
 * ------------------------------------------------------------------------- */
#define AIO_USB_IO_MODE_INPUT   0u
#define AIO_USB_IO_MODE_OUTPUT  1u

/* -------------------------------------------------------------------------
 * I/O configuration flags (aio_usb_io_config_t.flags)
 * ------------------------------------------------------------------------- */
#define AIO_USB_IO_FLAG_PULLUP     0x01u  /* enable internal pull-up   */
#define AIO_USB_IO_FLAG_PULLDOWN   0x02u  /* enable internal pull-down */
#define AIO_USB_IO_FLAG_ACTIVE_LOW 0x04u  /* line is active-low        */

/* -------------------------------------------------------------------------
 * Device feature flags (aio_usb_caps_t.features)
 * ------------------------------------------------------------------------- */
#define AIO_USB_FEATURE_TIMESTAMP   0x0001u  /* timestamps are valid           */
#define AIO_USB_FEATURE_AUTO_REPORT 0x0002u  /* periodic auto-report supported */
#define AIO_USB_FEATURE_ANALOG      0x0004u  /* analog channels are present    */

/* -------------------------------------------------------------------------
 * USB data structures (all packed to avoid padding)
 * ------------------------------------------------------------------------- */

/* Control OUT — AIO_USB_BREQ_HOST_FORMAT (host->device): byte-order handshake */
typedef struct TU_ATTR_PACKED
{
    uint32_t byte_order;
} aio_usb_host_config_t;

/* Control IN — AIO_USB_BREQ_DEVICE_CONFIG (device->host): capabilities */
typedef struct TU_ATTR_PACKED
{
    uint8_t  io_count;                /* number of digital I/O lines (<=32)    */
    uint8_t  analog_count;            /* number of analog channels (<=16)      */
    uint8_t  analog_resolution_bits;  /* max ADC resolution in bits            */
    uint8_t  reserved;
    uint32_t io_input_capable_mask;   /* bit i: line i can be an input         */
    uint32_t io_output_capable_mask;  /* bit i: line i can be an output        */
                                      /* (both set => bidirectional)           */
    uint32_t sw_version;
    uint32_t hw_version;
    uint32_t features;                /* AIO_USB_FEATURE_* bitmask             */
} aio_usb_caps_t;

/* Control OUT — AIO_USB_BREQ_IO_CONFIG (host->device): configure one I/O line.
 * wValue[7:0] carries the I/O line index. */
typedef struct TU_ATTR_PACKED
{
    uint8_t  mode;           /* AIO_USB_IO_MODE_INPUT / OUTPUT               */
    uint8_t  default_state;  /* initial output level (output mode only)      */
    uint8_t  flags;          /* AIO_USB_IO_FLAG_*                            */
    uint8_t  reserved;
    uint32_t auto_report_ms; /* periodic report interval; 0 = no auto-report */
} aio_usb_io_config_t;

/* Control OUT — AIO_USB_BREQ_IO_SET (host->device): set output line states.
 * Also accepted as an 8-byte bulk OUT frame for low-latency writes. */
typedef struct TU_ATTR_PACKED
{
    uint32_t mask;    /* which I/O lines to write    */
    uint32_t values;  /* desired levels for the mask */
} aio_usb_io_set_t;

/* Control IN — AIO_USB_BREQ_READ_STATUS (device->host) AND the bulk IN
 * auto-report frame: a full snapshot of every line and analog channel. */
typedef struct TU_ATTR_PACKED
{
    uint32_t timestamp_ms;                  /* HAL_GetTick() at sample time   */
    uint32_t io_states;                     /* level of all I/O lines (bit i) */
    uint32_t io_direction;                  /* bit i: 1=output, 0=input       */
    uint16_t analog[AIO_USB_ANALOG_COUNT];  /* analog values, right-aligned   */
} aio_usb_report_t;

/* -------------------------------------------------------------------------
 * Hardware backend hooks
 *
 * The driver calls these to talk to the real GPIO/ADC hardware.  Weak no-op
 * defaults are provided in aio_usb.c so the project links and enumerates
 * before any hardware is wired; override them in the application to drive
 * actual pins / ADC channels.
 * ------------------------------------------------------------------------- */

/* Apply direction / pull / default-state configuration to one I/O line. */
void aio_hw_config_io(uint8_t io, uint8_t mode, uint8_t flags, uint8_t default_state);

/* Drive the output lines selected by `mask` to the levels in `values`. */
void aio_hw_set_outputs(uint32_t mask, uint32_t values);

/* Return the current level of all I/O lines (bit i = line i). */
uint32_t aio_hw_read_inputs(void);

/* Return the latest raw value of analog channel `ch` (right-aligned). */
uint16_t aio_hw_read_analog(uint8_t ch);

/* Visual identify (flash an LED, etc.). */
void aio_hw_identify(void);

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Call once after tusb_init(). */
void aio_usb_init(void);

/*
 * Call in the main loop alongside tud_task().
 * Samples inputs/analog, emits due auto-report snapshots and pumps the
 * in-queue toward the host.
 */
void aio_usb_task(void);

/* Internal: vendor control handler called by the dispatcher in usb_app_drivers.c. */
bool aiousb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *req);

/*
 * USB bus suspend / resume, called by the tud_suspend_cb() / tud_resume_cb()
 * dispatcher in usb_app_drivers.c.  Auto-reports pause while suspended and
 * restart one period after resume.  Output levels are left as they are: they
 * are static and generate no traffic.
 */
void aio_usb_suspend(void);
void aio_usb_resume(void);
