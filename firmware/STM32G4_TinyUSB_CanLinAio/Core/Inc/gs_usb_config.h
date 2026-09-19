/*
 * gs_usb_config.h — compile-time configuration for the gs_usb CAN driver.
 *
 * Copyright (c) 2026 Schildkroet
 *
 * Edit this file to change the number of CAN channels or hardware
 * timing parameters.  Everything else in gs_usb.c / usb_descriptors.c
 * is derived from these defines automatically.
 */

#ifndef GS_USB_CONFIG_H_
#define GS_USB_CONFIG_H_

/*
 * Number of CAN channels exposed over USB.
 * The STM32G473 has FDCAN1, FDCAN2, FDCAN3 — valid range: 1–3.
 * The Linux gs_usb driver supports up to 3.
 */
#define GS_USB_CAN_CHANNEL_COUNT    2u

/*
 * FDCAN kernel clock in Hz.
 * PCLK1 = 96 MHz: HSE 8 MHz → PLL ×24 ÷2, APB1 prescaler = 1.
 * All channels share the same clock on STM32G4.
 * Reported to the host in BREQ_BT_CONST so it can compute bit timing.  This is
 * the default of the weak gs_engine_can_clock_hz(); an engine may override
 * that hook to read the configured clock tree instead.
 */
#define GS_USB_FDCAN_CLK_HZ         96000000UL

/*
 * Bulk endpoint addresses — must stay in sync with usb_descriptors.c and not
 * clash with lin_usb (EP3/EP4) or aio_usb (EP5/EP6).
 * EP_IN is device→host (CAN RX + TX echo), EP_OUT is host→device (CAN TX).
 */
#define GS_USB_EP_IN        0x81u
#define GS_USB_EP_OUT       0x02u

/*
 * STM32G4 FDCAN nominal bittiming constraints reported to the host.
 *   NominalTimeSeg1      = prop_seg + phase_seg1  → range 2–256
 *   NominalTimeSeg2      = phase_seg2             → range 2–128
 *   NominalSyncJumpWidth                          → range 1–128
 *   NominalPrescaler                              → range 1–512
 */
#define GS_USB_TSEG1_MIN        2u
#define GS_USB_TSEG1_MAX        256u
#define GS_USB_TSEG2_MIN        2u
#define GS_USB_TSEG2_MAX        128u
#define GS_USB_SJW_MAX          128u
#define GS_USB_BRP_MIN          1u
#define GS_USB_BRP_MAX          512u
#define GS_USB_BRP_INC          1u

/*
 * CAN FD data-phase constraints (FDCAN DBTP limits, IS_FDCAN_DATA_* in
 * stm32g4xx_hal_fdcan.h), reported in BREQ_BT_CONST_EXT.
 */
#define GS_USB_DTSEG1_MIN       1u
#define GS_USB_DTSEG1_MAX       32u
#define GS_USB_DTSEG2_MIN       1u
#define GS_USB_DTSEG2_MAX       16u
#define GS_USB_DSJW_MAX         16u
#define GS_USB_DBRP_MIN         1u
#define GS_USB_DBRP_MAX         32u
#define GS_USB_DBRP_INC         1u

/*
 * Feature flags advertised in BREQ_BT_CONST / BREQ_BT_CONST_EXT (GS_CAN_FEATURE_*
 * from gs_usb.h).  Advertise only what the CAN engine really implements.
 * The default below is the set the CANILFD engine supports; the weak stub
 * engine in gs_usb.c implements none of it, so trim it to your engine.
 *   - HW_TIMESTAMP, BT_CONST_EXT: handled by the transport, always safe.
 *   - FD: needs gs_engine_set_data_bittiming() and FD frame TX/RX.
 *   - GET_STATE: needs gs_engine_get_state().
 *   - AUTO_RESTART: private extension (bit 31), engine recovers from bus-off.
 *   - LOOP_BACK / ONE_SHOT / TRIPLE_SAMPLE: add if the engine honours the
 *     matching GS_CAN_FLAG_* in gs_engine_set_mode().
 */
#define GS_USB_FEATURES         (GS_CAN_FEATURE_LISTEN_ONLY  | \
                                 GS_CAN_FEATURE_HW_TIMESTAMP | \
                                 GS_CAN_FEATURE_IDENTIFY     | \
                                 GS_CAN_FEATURE_FD           | \
                                 GS_CAN_FEATURE_BT_CONST_EXT | \
                                 GS_CAN_FEATURE_GET_STATE    | \
                                 GS_CAN_FEATURE_AUTO_RESTART)

#endif /* GS_USB_CONFIG_H_ */
