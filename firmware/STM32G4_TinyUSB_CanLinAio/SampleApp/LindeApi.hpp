#pragma once

#include "lin_usb_protocol.h"
/* CMake sets the libusb include directory so <libusb.h> resolves on both
 * Linux (/usr/include/libusb-1.0) and Windows (vcpkg / manual install). */
#include <libusb.h>
#include <cstdint>
#include <deque>
#include <string>

/*
 * LindeApi — C++ host driver for the lin_usb USB LIN adapter.
 *
 * Usage:
 *   LindeApi api;
 *   api.open();                          // find device by VID/PID
 *   api.getDeviceConfig(cfg);            // query capabilities
 *   api.setBusConfig(...);                // configure channel
 *   api.setMode(LIN_USB_MODE_START);  // role (master/slave) set via setBusConfig()
 *   api.uploadScheduleEntry(...); api.scheduleStart(...);
 *   api.setFrame(frame);                 // set a publisher's payload, wait for ACK
 *   api.getFrame(frame, 200);            // read next scheduled result (200 ms)
 *   api.close();
 *
 * All methods return true on success, false on USB or protocol error.
 * Call lastError() to retrieve the most recent libusb error string.  Control
 * requests with out-of-range arguments (table_id, slot, dlc) are STALLed by
 * the device and fail with "request rejected by device (STALL)".
 */
class LindeApi
{
public:
    explicit LindeApi(uint8_t channel = 0);
    ~LindeApi();

    LindeApi(const LindeApi &) = delete;
    LindeApi &operator=(const LindeApi &) = delete;

    /* ---- Device lifecycle ---- */

    /* Open the first matching device. vid/pid default to the firmware values.
     * Also queries device config and validates that channel_ < channelCount(). */
    bool open(uint16_t vid = LIN_USB_VID, uint16_t pid = LIN_USB_PID);
    void close();
    bool isOpen() const { return dev_ != nullptr; }

    /* Number of LIN channels reported by the device; valid only after open(). */
    uint8_t channelCount() const { return channel_count_; }

    /* ---- Informational ---- */
    bool getDeviceConfig(lin_usb_device_config_t &cfg);
    bool getTimestamp(uint32_t &ts_ms);
    bool getBusState(lin_usb_bus_state_t &state);

    /* ---- Channel configuration ---- */
    bool setHostFormat();   /* send byte-order handshake */
    bool setBusConfig(const lin_usb_bus_config_t &cfg);
    bool setMode(uint8_t mode);
    bool identify();

    /* ---- Schedule management ---- */

    /* Upload one entry into a schedule table slot. */
    bool uploadScheduleEntry(uint8_t table_id, uint8_t slot,
                              const lin_usb_schedule_entry_t &entry);

    /* Update the TX payload of an existing entry (matched by lin_id). */
    bool updateFrameConfig(const lin_usb_schedule_entry_t &entry);

    /* Start / stop / pause a schedule table. */
    /* Convenience: start table `table_id` with `entry_count` entries. */
    bool scheduleStart(uint8_t table_id, uint8_t entry_count);
    bool scheduleStop();
    bool schedulePause();

    /* ---- Sleep / wakeup ---- */
    bool goToSleep();
    bool wakeup();

    /* ---- Bulk frame I/O (self-scheduling device) ---- */

    /*
     * Set the TX payload of the scheduled publisher frame whose LIN ID matches
     * frame.lin_id on this object's channel.  The device is self-scheduling, so
     * the new data is sent on that entry's next slot.
     *
     * The device acknowledges every such packet on the bulk IN pipe
     * (echo_id == LIN_USB_ECHO_ID_SET_DATA_ACK).  setFrame() waits up to
     * ack_timeout_ms for it and returns false if the device rejected the update
     * (LIN ID not in the running schedule, or invalid dlc).  Bus frames that
     * arrive while waiting are kept and returned by the next getFrame() calls.
     */
    bool setFrame(const lin_usb_host_frame_t &frame, unsigned ack_timeout_ms = 200);

    /*
     * Block until the next bus event (published echo or subscriber RX) arrives
     * from the device, or timeout_ms elapses.  Set-data ACKs are never
     * returned here.  Returns false on timeout or USB error.
     */
    bool getFrame(lin_usb_host_frame_t &frame, unsigned timeout_ms = 100);

    /* ---- Diagnostics ---- */
    const std::string &lastError() const { return last_error_; }

private:
    bool controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool controlIn (uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool setError(int libusb_rc, const char *context);
    bool checkChannel() const;   /* validates channel_ < channel_count_ */
    bool readIn(lin_usb_host_frame_t &frame, unsigned timeout_ms);  /* one bulk IN packet */

    libusb_context       *ctx_                    = nullptr;
    libusb_device_handle *dev_                    = nullptr;
    uint8_t               ep_in_                  = 0;
    uint8_t               ep_out_                 = 0;
    uint8_t               itf_                    = 0;
    uint8_t               channel_                = 0;
    uint8_t               channel_count_          = 0;
    bool                  kernel_driver_detached_ = false;
    std::deque<lin_usb_host_frame_t> pending_;   /* bus events read by setFrame() */
    std::string           last_error_;
};
