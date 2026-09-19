#pragma once

#include "gs_usb_protocol.h"
#include <libusb.h>
#include <cstdint>
#include <string>

/*
 * CandeApi — C++ host driver for the gs_usb (CAN) USB adapter.
 *
 * Unlike LindeApi (which takes a fixed channel at construction), CandeApi
 * manages the whole physical device.  Channel-specific methods take a uint8_t
 * ch argument; the valid range is 0 .. channelCount()-1.  This mirrors the
 * CandleApiDriver CandleSharedDevice model.
 *
 * Usage:
 *   CandeApi can;
 *   can.open();
 *   can.setHostFormat();
 *   can.getDeviceConfig(cfg);        // also sets channelCount()
 *   can.getBitTimingConst(0, bt);    // query clock and limits for ch 0
 *   can.setBitTiming(0, timing);     // 500 kbps on 96 MHz
 *   can.setDataBitTiming(0, dtiming); // CAN FD only: data phase, e.g. 2 Mbit/s
 *   can.setMode(0, GS_CAN_MODE_START, GS_CAN_FLAG_HW_TIMESTAMP | GS_CAN_FLAG_FD);
 *   can.sendFrame(frame);            // classic, or FD with GS_FRAME_FLAG_FD
 *   can.receiveFrame(frame, 200);    // 200 ms timeout
 *
 * Frame payload lives in frame.classic.data[] (classic) or frame.fd.data[]
 * (GS_FRAME_FLAG_FD, can_dlc is then a DLC code 0–15, see gs_dlc_to_len()).
 *   can.setMode(0, GS_CAN_MODE_RESET, 0);
 *   can.close();
 */
class CandeApi
{
public:
    CandeApi();
    ~CandeApi();

    CandeApi(const CandeApi &) = delete;
    CandeApi &operator=(const CandeApi &) = delete;

    /* ---- Device lifecycle ---- */

    /* Open first device matching vid/pid and claim the CAN interface.
     * Automatically queries device config and sets channelCount(). */
    bool open(uint16_t vid = GS_USB_VID, uint16_t pid = GS_USB_PID);
    void close();
    bool isOpen() const { return dev_ != nullptr; }

    /* Number of CAN channels on this device.  Valid only after open(). */
    uint8_t channelCount() const { return channel_count_; }

    /* ---- Device-level queries ---- */
    bool getDeviceConfig(gs_device_config_t &cfg);
    /* Device time in microseconds (same clock as frame timestamp_us). */
    bool getTimestamp(uint32_t &ts_us);

    /* Query timing constraints and feature flags for channel ch. */
    bool getBitTimingConst(uint8_t ch, gs_device_bt_const_t &bt);

    /* Same plus the CAN FD data-phase limits.  Only valid when
     * GS_CAN_FEATURE_BT_CONST_EXT is advertised. */
    bool getBitTimingConstExt(uint8_t ch, gs_device_bt_const_ext_t &bt);

    /* Channel state (GS_CAN_STATE_*) and error counters.  Only valid when
     * GS_CAN_FEATURE_GET_STATE is advertised. */
    bool getState(uint8_t ch, gs_device_state_t &state);

    /* Send byte-order handshake (call once after open, before any config). */
    bool setHostFormat();

    /* ---- Channel-level configuration (ch must be < channelCount()) ---- */

    /* Configure nominal bit timing.  Use getBitTimingConst() to obtain
     * the valid brp / tseg / sjw ranges for the target device. */
    bool setBitTiming(uint8_t ch, const gs_device_bittiming_t &bt);

    /* Configure the CAN FD data-phase bit timing (used on the next START with
     * GS_CAN_FLAG_FD).  Limits come from getBitTimingConstExt(). */
    bool setDataBitTiming(uint8_t ch, const gs_device_bittiming_t &bt);

    /* Start (GS_CAN_MODE_START) or stop (GS_CAN_MODE_RESET) a channel.
     * Pass GS_CAN_FLAG_* in flags (e.g. GS_CAN_FLAG_FD); only use flags whose
     * GS_CAN_FEATURE_* bit the device advertises. */
    bool setMode(uint8_t ch, uint32_t mode, uint32_t flags);

    /* Flash the LED to confirm which device is connected. */
    bool identify(uint8_t ch);

    /* ---- Bulk frame I/O ---- */

    /* Transmit a frame.  frame.channel must be < channelCount().  Sends the
     * 20-byte classic or, with GS_FRAME_FLAG_FD, the 76-byte FD layout. */
    bool sendFrame(const gs_host_frame_t &frame);

    /* Block until a frame arrives from any channel or timeout_ms elapses.
     * On success, frame.channel identifies the originating channel.  The
     * timestamp_us field is only filled if the channel was started with
     * GS_CAN_FLAG_HW_TIMESTAMP (otherwise it reads 0).
     * Returns false on timeout (lastError() == "receive timeout") or error. */
    bool receiveFrame(gs_host_frame_t &frame, unsigned timeout_ms = 100);

    /* ---- Diagnostics ---- */
    const std::string &lastError() const { return last_error_; }

private:
    bool controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool controlIn (uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool setError(int libusb_rc, const char *context);
    bool checkChannel(uint8_t ch);

    libusb_context       *ctx_                    = nullptr;
    libusb_device_handle *dev_                    = nullptr;
    uint8_t               ep_in_                  = 0;
    uint8_t               ep_out_                 = 0;
    uint8_t               itf_                    = 0;
    uint8_t               channel_count_          = 0;
    bool                  kernel_driver_detached_ = false;
    std::string           last_error_;
};
