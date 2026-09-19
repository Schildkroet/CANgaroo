#pragma once

/*
 * aio_usb_config.h — compile-time configuration for the aio_usb driver.
 *
 * aio_usb exposes a generic digital I/O + analog-input device over a
 * vendor-specific USB interface (bInterfaceProtocol = 0x02), alongside the
 * gs_usb (CAN, 0xFF) and lin_usb (LIN, 0x01) interfaces.
 */

/* Number of digital I/O lines (max 32 — they are addressed by a uint32_t mask) */
#define AIO_USB_IO_COUNT          32u

/* Number of analog input channels (max 16 — sized into aio_usb_report_t) */
#define AIO_USB_ANALOG_COUNT      16u

/* Maximum analog resolution in bits reported to the host (e.g. 16-bit ADC) */
#define AIO_USB_ANALOG_RES_BITS   16u

/*
 * Capability masks: bit i set means I/O line i can act as an input / output.
 * A line with both bits set is bidirectional.  The defaults advertise every
 * line as fully bidirectional; narrow these to match real hardware.
 */
#define AIO_USB_IO_INPUT_MASK     0xFFFFFFFFu
#define AIO_USB_IO_OUTPUT_MASK    0xFFFFFFFFu

/* Bulk endpoint addresses — must not clash with gs_usb (EP1/EP2) or
 * lin_usb (EP3/EP4). */
#define AIO_USB_EP_IN             0x85u  /* device -> host: EP5 IN  */
#define AIO_USB_EP_OUT            0x06u  /* host -> device: EP6 OUT */

/* Circular queue depth for report frames waiting to be sent to the host */
#define AIO_USB_IN_QUEUE_SIZE     8u

/* Software / hardware version reported to the host */
#define AIO_USB_SW_VERSION        1u
#define AIO_USB_HW_VERSION        1u
