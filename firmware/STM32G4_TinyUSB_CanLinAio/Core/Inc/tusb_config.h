/*
 * tusb_config.h — TinyUSB 0.21 configuration for the STM32G473 USB device
 * controller.
 *
 * Device stack only (this MCU has no USB host/OTG peripheral). Every built-in
 * CFG_TUD_* class flag is 0: the three interfaces this firmware exposes
 * (gs_usb / lin_usb / aio_usb) are custom class drivers registered through
 * usbd_app_driver_get_cb() in usb_app_drivers.c, not TinyUSB's standard class
 * stack, so CFG_TUD_VENDOR stays 0 too.
 */
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif


//--------------------------------------------------------------------+
// Common configuration
//--------------------------------------------------------------------+

// STM32G473 has the "fsdev"-family full-speed device controller
// (src/portable/st/stm32_fsdev), not the OTG core — no host mode available.
#define CFG_TUSB_MCU            OPT_MCU_STM32G4

// Bare-metal superloop, no RTOS
#define CFG_TUSB_OS             OPT_OS_NONE

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

// fsdev packet memory (PMA) has its own bus width/alignment; no special
// linker section needed on this MCU.
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))


//--------------------------------------------------------------------+
// Device configuration
//--------------------------------------------------------------------+

#define CFG_TUD_ENABLED         1
#define CFG_TUH_ENABLED         0

// Full speed only — the fsdev controller has no high-speed PHY
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED

#define CFG_TUD_ENDPOINT0_SIZE  64

//------------- Class drivers — none enabled -------------//
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_MIDI2           0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_AUDIO           0
#define CFG_TUD_VIDEO           0
#define CFG_TUD_USBTMC          0
#define CFG_TUD_DFU              0
#define CFG_TUD_DFU_RUNTIME      0
#define CFG_TUD_ECM_RNDIS        0
#define CFG_TUD_NCM              0
#define CFG_TUD_BTH              0
#define CFG_TUD_PRINTER          0
#define CFG_TUD_MTP              0


#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
