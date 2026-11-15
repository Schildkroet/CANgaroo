/*

  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

#pragma once

// Simulated gs_usb adapter at the USB level: vendor control requests and bulk
// transfers, as candle.c sends and receives them through candle_usb.h. Two
// firmwares, transcribed from their sources:
//
//   CandleLightF072  github.com/candle-usb/candleLight_fw 85e5b33, bxCAN board
//                    (48 MHz, classic CAN): src/usbd_gs_can.c, src/can_common.c,
//                    src/can/bxcan.c, include/gs_usb.h
//   ElmueG431        github.com/Elmue/CANable-2.5-firmware-Slcan-and-Candlelight
//                    12aa9fa, Candlelight G431 Multiboard (160 MHz, CAN FD), legacy
//                    gs_usb protocol: Firmware/Candlelight/control.c, buffer.c,
//                    candlelight_def.h, Firmware/can.c
//
// Never adjust this to what cangaroo expects (see issue #34).
//
// What a host must know about both:
//   - A request is refused in its SETUP stage by a STALL (the control transfer
//     fails): unknown request, invalid channel, wLength too short. A request
//     whose OUT data is refused (bad bit timing, START while running, recovery
//     when not bus-off) is NOT reported: the ST USB stack acknowledges the data
//     stage whatever the handler returns (usbd_core.c: USBD_LL_DataOutStage;
//     Elmue says so in control_setup_OUT_data()).
//   - A sent frame is echoed with the host's echo_id as soon as it is in the CAN
//     controller's TX buffer, not when it was ACKed on the bus.
//   - Frames to the host: classic 20 bytes, FD 76, plus 4 for a hardware
//     timestamp when the channel was started with it; upstream pads classic
//     frames to 64 when started with PAD_PKTS_TO_MAX_PKT_SIZE.
//
// Differences:
//   - Upstream F072: no CAN FD (requests 10/11/17-19 stall), BERR reporting,
//     GET_STATE, BUS_OFF_RECOVERY; error frames on state changes and bus-off.
//   - Elmue G431: CAN FD, GET_STATE; no BERR, TDC or BUS_OFF_RECOVERY (those
//     requests stall), no error frames in legacy mode. START while open, a bit
//     timing while open, or START without a bit timing are ignored silently;
//     closing a channel forgets its bit timings (can_reset). In listen only, and
//     while closed, frames from the host are dropped without an echo.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <QByteArray>
#include <QList>

class FakeGsUsbDevice
{
public:
    enum class Firmware { CandleLightF072, ElmueG431 };

    struct Frame
    {
        uint32_t id{0};
        bool extended{false};
        bool rtr{false};
        bool fd{false};
        bool brs{false};
        uint8_t dlc{0};
        QByteArray data;
    };

    // struct gs_device_bittiming
    struct Timing
    {
        uint32_t prop{0};
        uint32_t seg1{0};
        uint32_t seg2{0};
        uint32_t sjw{0};
        uint32_t brp{0};

        [[nodiscard]] uint32_t quanta() const { return 1 + prop + seg1 + seg2; }
    };

    // gs_can_state
    enum class State : uint32_t { ErrorActive = 0, ErrorWarning, ErrorPassive, BusOff, Stopped };

    explicit FakeGsUsbDevice(Firmware fw, int channels = 1, std::wstring path = L"fake-gsusb-0");
    ~FakeGsUsbDevice();
    FakeGsUsbDevice(const FakeGsUsbDevice &) = delete;
    FakeGsUsbDevice &operator=(const FakeGsUsbDevice &) = delete;

    // Devices candle_usb_scan() finds (test transport, FakeCandleUsb.cpp)
    static std::vector<FakeGsUsbDevice *> &present();
    static FakeGsUsbDevice *byPath(const wchar_t *path);

    // ── Test controls ────────────────────────────────────────────────────────
    // Another program holds the device (WinUSB opens exclusively): open fails.
    void setInUse(bool inUse);
    // USB gone: every transfer fails from now on.
    void unplug();
    // A frame on the bus; reported if the channel is started.
    void receiveBusFrame(int ch, const Frame &frame);
    // Error counters; the state follows (96 warning, 128 passive, 256 bus off).
    void setErrorCounters(int ch, uint32_t txErr, uint32_t rxErr);
    // Mark the next frame to the host with GS_CAN_FLAG_OVERFLOW.
    void overflowNext() { std::lock_guard lock(_mutex); _overflowNext = true; }
    // State an earlier session left: channel started with this nominal timing.
    void leaveRunning(int ch, const Timing &nominal);

    // ── Inspection ───────────────────────────────────────────────────────────
    [[nodiscard]] Firmware firmware() const { return _fw; }
    [[nodiscard]] const std::wstring &path() const { return _path; }
    [[nodiscard]] uint32_t clockHz() const;
    [[nodiscard]] bool started(int ch) const;
    [[nodiscard]] uint32_t startFlags(int ch) const;
    [[nodiscard]] std::optional<Timing> nominalTiming(int ch) const;
    [[nodiscard]] std::optional<Timing> dataTiming(int ch) const;
    [[nodiscard]] bool listenOnly(int ch) const;
    [[nodiscard]] QList<Frame> transmitted(int ch) const;
    // Requests the device stalled (bRequest values)
    [[nodiscard]] QList<uint8_t> stalled() const;
    // OUT requests whose data the device ignored (bRequest values)
    [[nodiscard]] QList<uint8_t> ignored() const;
    // Bulk IN transfers the host started (a read loop without a pause on
    // errors shows up here)
    [[nodiscard]] int bulkInCalls() const;
    [[nodiscard]] int usbOpenCount() const;

    // ── USB side, for the test transport ─────────────────────────────────────
    bool usbOpen();
    void usbClose();
    bool control(uint8_t request, uint8_t requesttype, uint16_t value, uint16_t index, uint8_t *data, uint16_t size);
    bool bulkOut(const uint8_t *data, uint32_t len);
    // 0 = ok, 1 = timeout, 2 = transfer failed
    int bulkIn(uint8_t *buf, uint32_t bufsize, uint32_t *len, uint32_t timeoutMs);

private:
    struct Channel
    {
        bool started{false};
        uint32_t flags{0};
        std::optional<Timing> nominal;
        std::optional<Timing> data;
        uint32_t txErr{0};
        uint32_t rxErr{0};
        State state{State::Stopped};
        QList<Frame> transmitted;
        std::deque<QByteArray> pendingTx;  // upstream: queued while not started
    };

    [[nodiscard]] uint32_t feature() const;
    [[nodiscard]] uint32_t timestampUs() const;
    bool setupStage(uint8_t request, bool in, uint16_t value, uint16_t size);
    void dataStage(uint8_t request, uint16_t value, const uint8_t *data, uint16_t size);
    bool inData(uint8_t request, uint16_t value, uint8_t *data, uint16_t size);
    bool timingOk(bool dataPhase, const Timing &t) const;
    void startChannel(int ch, uint32_t flags);
    void stopChannel(int ch);
    void transmit(int ch, const QByteArray &hostFrame);
    void toHost(int ch, QByteArray frame);  // completes size, timestamp, overflow flag
    void errorFrame(int ch, uint32_t canIdClasses, uint8_t data1);
    void updateState(int ch);

    Firmware _fw;
    std::wstring _path;
    mutable std::mutex _mutex;
    std::condition_variable _cond;
    std::chrono::steady_clock::time_point _t0{std::chrono::steady_clock::now()};
    std::vector<Channel> _ch;
    std::deque<QByteArray> _toHost;
    bool _inUse{false};
    bool _unplugged{false};
    bool _overflowNext{false};
    int _usbOpen{0};
    int _bulkInCalls{0};
    QList<uint8_t> _stalled;
    QList<uint8_t> _ignored;
};
