#pragma once

/*
 * usb_app_config.h — select which USB bus drivers are compiled in.
 *
 * Set each flag to 1 to include the driver or 0 to exclude it.
 * At least one driver must be enabled.
 *
 * Interface numbers follow from these flags (CAN 0, then LIN, then AIO).  The
 * Linux gs_usb driver binds interface 0 of the candleLight VID/PID used in
 * usb_descriptors.c, so a build without CAN needs its own VID/PID.
 */

#define GS_USB_ENABLED   1   /* CAN interface via gs_usb  */
#define LIN_USB_ENABLED  1   /* LIN interface via lin_usb */
#define AIO_USB_ENABLED  1   /* I/O + analog interface via aio_usb */

#if !GS_USB_ENABLED && !LIN_USB_ENABLED && !AIO_USB_ENABLED
#error "usb_app_config.h: at least one driver must be enabled"
#endif

/* MS OS 2.0 descriptor set: Windows loads WinUSB for every interface without an
 * INF, via one function subset per enabled interface. The vendor code is the
 * bRequest value Windows uses to retrieve the set; it is sent with device
 * recipient, so it cannot collide with the per-interface vendor requests. */
#define MS_OS_20_VENDOR_CODE  0x01u
#define MS_OS_20_FN_LEN       156u  /* fn subset header(8) + compatID(20) + regProp(128) */
#define MS_OS_20_DESC_LEN     (10u + 8u + MS_OS_20_FN_LEN * \
                               (GS_USB_ENABLED + LIN_USB_ENABLED + AIO_USB_ENABLED))
