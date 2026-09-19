#pragma once

/* Number of LIN channels exposed over USB */
#define LIN_USB_CHANNEL_COUNT           2u

/* Bulk endpoint addresses — must not clash with gs_usb (EP1 IN=0x81, OUT=0x02)
 * or aio_usb (EP5/EP6). */
#define LIN_USB_EP_IN                   0x83u  /* device -> host: EP3 IN */
#define LIN_USB_EP_OUT                  0x04u  /* host -> device: EP4 OUT */

/* Maximum number of independent schedule tables per channel */
#define LIN_USB_MAX_SCHEDULE_TABLES     4u

/* Maximum entries per schedule table (reported to the host in DEVICE_CONFIG) */
#define LIN_USB_MAX_SCHEDULE_ENTRIES    16u

/* Circular queue depth for frames waiting to be sent to the host. Frames that
 * do not fit are counted per channel and reported in the BUS_STATE reply. */
#define LIN_USB_IN_QUEUE_SIZE           32u

/* Software / hardware version reported to the host.
 * SW version is packed as major << 16 | minor << 8 | patch (0.1.0 = 0x000100);
 * hosts show it as "major.minor.patch". */
#define LIN_USB_VERSION(major, minor, patch)  (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | (uint32_t)(patch))
#define LIN_USB_SW_VERSION              LIN_USB_VERSION(0u, 1u, 0u)
#define LIN_USB_HW_VERSION              1u

/*
 * Bitmask of standard LIN baud rates the hardware supports.
 * Use the raw bit positions defined by LIN_USB_BAUD_* in lin_usb.h:
 *   0x08 = 9600, 0x10 = 10417, 0x20 = 19200 (see lin_usb.h for all values)
 */
#define LIN_USB_SUPPORTED_BAUDRATES     (LIN_USB_BAUD_2400 | \
                                        LIN_USB_BAUD_4800 | \
                                        LIN_USB_BAUD_9600 | \
                                        LIN_USB_BAUD_10417 | \
                                        LIN_USB_BAUD_19200 | \
                                        LIN_USB_BAUD_20000)

/*
 * Set to 1 if the UART peripheral accepts arbitrary baud rates
 * beyond those listed in LIN_USB_SUPPORTED_BAUDRATES.
 */
#define LIN_USB_CUSTOM_BAUDRATE_SUPPORTED   1
