#pragma once

#include "aio_usb_protocol.h"
/* CMake sets the libusb include directory so <libusb.h> resolves on both
 * Linux (/usr/include/libusb-1.0) and Windows (vcpkg / manual install). */
#include <libusb.h>
#include <cstdint>
#include <string>

/*
 * AiodeApi — C++ host driver for the aio_usb digital-I/O + analog adapter.
 *
 * Usage:
 *   AiodeApi api;
 *   api.open();                                  // find device by VID/PID
 *   api.getCaps(caps);                           // query capabilities
 *   api.configIo(0, AIO_USB_IO_MODE_OUTPUT, 0, 0, 0);
 *   api.setOutputs(0x1, 0x1);                    // drive line 0 high
 *   aio_usb_report_t st;
 *   api.readStatus(st);                          // poll a snapshot
 *   api.receiveReport(st, 200);                  // wait for an auto-report
 *   api.close();
 *
 * All methods return true on success, false on USB or protocol error.
 * Call lastError() to retrieve the most recent libusb error string.
 */
class AiodeApi
{
public:
    AiodeApi();
    ~AiodeApi();

    AiodeApi(const AiodeApi &) = delete;
    AiodeApi &operator=(const AiodeApi &) = delete;

    /* ---- Device lifecycle ---- */

    /* Open the first matching device. vid/pid default to the firmware values. */
    bool open(uint16_t vid = AIO_USB_VID, uint16_t pid = AIO_USB_PID);
    void close();
    bool isOpen() const { return dev_ != nullptr; }

    /* ---- Informational ---- */
    bool getCaps(aio_usb_caps_t &caps);
    bool getTimestamp(uint32_t &ts_ms);

    /* ---- Configuration ---- */
    bool setHostFormat();   /* send byte-order handshake */
    bool identify();

    /* Configure one I/O line (direction, default level, pull, auto-report). */
    bool configIo(uint8_t io, uint8_t mode, uint8_t default_state,
                  uint8_t flags, uint32_t auto_report_ms);

    /* ---- Output control ---- */

    /* Drive the lines in `mask` to the levels in `values` (control transfer). */
    bool setOutputs(uint32_t mask, uint32_t values);

    /* Same as setOutputs() but over the bulk OUT endpoint (low latency). */
    bool setOutputsBulk(uint32_t mask, uint32_t values);

    /* ---- Status ---- */

    /* Poll a full snapshot of all I/O lines and analog channels. */
    bool readStatus(aio_usb_report_t &status);

    /*
     * Block until an auto-report frame arrives from the device or timeout_ms
     * elapses.  Returns false on timeout or USB error.
     */
    bool receiveReport(aio_usb_report_t &report, unsigned timeout_ms = 100);

    /* ---- Diagnostics ---- */
    const std::string &lastError() const { return last_error_; }

private:
    bool controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool controlIn (uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool setError(int libusb_rc, const char *context);

    libusb_context       *ctx_                    = nullptr;
    libusb_device_handle *dev_                    = nullptr;
    uint8_t               ep_in_                  = 0;
    uint8_t               ep_out_                 = 0;
    uint8_t               itf_                    = 0;
    bool                  kernel_driver_detached_ = false;
    std::string           last_error_;
};
