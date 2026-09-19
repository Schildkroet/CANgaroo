/*
 * usb_app_drivers.c
 *
 * Single point of registration for all application-level TinyUSB class drivers.
 * Which drivers are compiled in is controlled by GS_USB_ENABLED / GS_LIN_ENABLED
 * in usb_app_config.h.
 *
 * Why a separate file:
 *   TinyUSB v0.20.0 routes ALL vendor control requests through the single
 *   weak symbol tud_vendor_control_xfer_cb() (usbd.c:789), bypassing the
 *   per-driver control_xfer_cb field.  With two drivers both defining that
 *   symbol the linker would see a duplicate definition.  This file defines
 *   the symbol once and dispatches to each enabled driver's internal handler.
 *
 *   usbd_app_driver_get_cb() must also be defined exactly once; it returns
 *   a pointer to a contiguous array of usbd_class_driver_t that TinyUSB
 *   iterates with array indexing.
 */

#include "usb_app_config.h"
#include "device/usbd_pvt.h"

#if GS_USB_ENABLED
#include "gs_usb.h"
extern usbd_class_driver_t const gs_usb_driver;
bool gsusb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *request);
#endif

#if LIN_USB_ENABLED
#include "lin_usb.h"
extern usbd_class_driver_t const lin_usb_driver;
bool linusb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                             tusb_control_request_t const *req);
#endif

#if AIO_USB_ENABLED
#include "aio_usb.h"
extern usbd_class_driver_t const aio_usb_driver;
bool aiousb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *req);
#endif

/* -------------------------------------------------------------------------
 * Contiguous driver table required by TinyUSB's array-indexed iteration.
 * Structs are copied once at first use; C static initializers cannot
 * reference extern objects at compile time.
 * ------------------------------------------------------------------------- */
#define _DRIVER_COUNT  (GS_USB_ENABLED + LIN_USB_ENABLED + AIO_USB_ENABLED)

static usbd_class_driver_t _all_drivers[_DRIVER_COUNT];
static bool _drivers_ready;

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    if (!_drivers_ready) {
        uint8_t idx = 0;
#if GS_USB_ENABLED
        _all_drivers[idx++] = gs_usb_driver;
#endif
#if LIN_USB_ENABLED
        _all_drivers[idx++] = lin_usb_driver;
#endif
#if AIO_USB_ENABLED
        _all_drivers[idx++] = aio_usb_driver;
#endif
        _drivers_ready = true;
    }
    *driver_count = _DRIVER_COUNT;
    return _all_drivers;
}

/* -------------------------------------------------------------------------
 * Single vendor-control dispatcher.
 * Each handler returns true only when wIndex matches its own interface,
 * so at most one driver claims any given request.
 * ------------------------------------------------------------------------- */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 tusb_control_request_t const *request)
{
    /* MS OS 2.0 descriptor request: device-to-host, vendor, device recipient, wIndex = 7.
     * Windows sends this after reading the BOS platform capability to retrieve the
     * WinUSB CompatibleIDs that auto-load the driver for each interface. */
    extern uint8_t const desc_ms_os_20[MS_OS_20_DESC_LEN];
    if (request->bmRequestType_bit.type      == TUSB_REQ_TYPE_VENDOR     &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE     &&
        request->bRequest                    == MS_OS_20_VENDOR_CODE      &&
        request->wIndex                      == 0x0007u) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, request, (void *)desc_ms_os_20,
                                    TU_MIN(request->wLength, MS_OS_20_DESC_LEN));
        }
        return true;
    }

#if GS_USB_ENABLED
    if (gsusb_control_xfer_cb(rhport, stage, request)) return true;
#endif
#if LIN_USB_ENABLED
    if (linusb_control_xfer_cb(rhport, stage, request)) return true;
#endif
#if AIO_USB_ENABLED
    if (aiousb_control_xfer_cb(rhport, stage, request)) return true;
#endif
    return false;
}

/* -------------------------------------------------------------------------
 * USB bus suspend / resume.  TinyUSB has one weak callback for each, so, like
 * the vendor requests above, they are defined once here and dispatched.
 * Both run from tud_task() (main-loop context).
 * ------------------------------------------------------------------------- */
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
#if GS_USB_ENABLED
    gs_usb_suspend();
#endif
#if LIN_USB_ENABLED
    lin_usb_suspend();
#endif
#if AIO_USB_ENABLED
    aio_usb_suspend();
#endif
}

void tud_resume_cb(void)
{
#if GS_USB_ENABLED
    gs_usb_resume();
#endif
#if LIN_USB_ENABLED
    lin_usb_resume();
#endif
#if AIO_USB_ENABLED
    aio_usb_resume();
#endif
}
