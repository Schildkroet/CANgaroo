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

// The lin_usb interface of the STM32G4 CAN/LIN/AIO firmware, transcribed from
// firmware/STM32G4_TinyUSB_CanLinAio/Core/Src/lin_usb.c and lin_usb_config.h,
// not from the cangaroo driver:
//   - vendor control requests: STALL in SETUP for a channel >= 2, a schedule
//     slot >= 16, an unknown request; STALL in the DATA stage for MODE START
//     with table_id >= 4 and FRAME_CONFIG / SCHEDULE with table_id >= 4 or a
//     dlc outside 1..8
//   - DEVICE_CONFIG: 4 tables, 16 entries, baud mask 0x7E, icount 1, sw 0.1.0,
//     hw 1, features SCHEDULING | CUSTOM_BAUDRATE | TIMESTAMP | BUS_STATE |
//     LISTEN_ONLY
//   - bulk OUT "set frame": exactly one ACK (echo_id 0) per packet, the packet
//     echoed (zeroed if short), flags ERROR unless 20 bytes, channel < 2,
//     dlc 1..8 and lin_engine_set_data() true. If the IN queue is full the ACK
//     is held and the OUT endpoint stays un-armed (host NAKed) until
//     lin_usb_task() queues it
//   - bulk IN: a 32-slot ring (31 usable) plus the frame in flight; bus frames
//     that do not fit are counted per channel for BUS_STATE.dropped
//
// lin_usb_config.h is per board: Config::reference() is the repository's,
// Config::canilfd() what the CANILFD adapter (Hermelin Labs, bcdDevice 1.03)
// reports.
//
// The LIN engine behind the lin_engine_* hooks is not part of the repository
// (lin_usb.c only has weak stubs). Its behaviour here was measured on the
// CANILFD adapter with tests/hw_compare (linusb-engine), see the README there:
//   - BUS_STATE STOPPED until MODE START, OK when started, SLEEPING after sleep
//   - set data is accepted (ACK flags 0) only while started and only for a LIN
//     ID in the active table's first entry_count slots, publisher or not; it
//     replaces that slot's payload
//   - frames carry the protected ID; a published frame has RESPONDED | VALID,
//     a subscriber slot nobody answers SUBSCRIBER | ERROR with zeroed data
//   - sleep while started: a frame id 0, dlc 8, 00 FF FF.., flags SLEEP;
//     wakeup: id 0, dlc 0, flags WAKEUP; on a stopped channel both do nothing
//     (CANILFD firmware with the LinIF stopped-channel fix)
// Every hook call is recorded as well, and tests can report frames directly.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include <QList>

#include "driver/LindeApiDriver/lin_usb_protocol.h"

class FakeLinUsbDevice
{
public:
    static constexpr int k_channels = 2;          // LIN_USB_CHANNEL_COUNT
    static constexpr int k_maxTables = 8;         // storage; Config::tables is the limit
    static constexpr int k_entries = 16;          // LIN_USB_MAX_SCHEDULE_ENTRIES
    static constexpr int k_inQueueSize = 32;      // LIN_USB_IN_QUEUE_SIZE
    static constexpr uint8_t k_interface = 1;     // bInterfaceNumber (gs_usb is 0)

    struct Config
    {
        uint8_t tables;      // LIN_USB_MAX_SCHEDULE_TABLES
        uint8_t baudrates;   // LIN_USB_SUPPORTED_BAUDRATES
        uint32_t hwVersion;  // LIN_USB_HW_VERSION

        static Config reference() { return {.tables = 4, .baudrates = 0x7E, .hwVersion = 1}; }
        static Config canilfd() { return {.tables = 6, .baudrates = 0x7F, .hwVersion = 2}; }
    };

    enum class Result
    {
        Ok,
        Stall,
        Timeout,
        NoDevice,
    };

    // What the LIN engine was told through the lin_engine_* hooks, per channel.
    struct Channel
    {
        std::optional<lin_usb_bus_config_t> config;       // lin_engine_configure
        std::optional<lin_usb_mode_t> mode;               // lin_engine_set_mode, last call
        std::optional<lin_usb_schedule_entry_t> schedule[k_maxTables][k_entries];
        std::vector<lin_usb_schedule_entry_t> frameUpdates;  // lin_engine_update_frame
        std::vector<lin_usb_host_frame_t> setData;        // lin_engine_set_data
        std::vector<uint8_t> sleepWakeup;                 // lin_engine_sleep_wakeup commands
        int identifies{0};
        uint8_t busState{LIN_USB_BUS_STATE_STOPPED};      // lin_engine_get_bus_state
    };

    explicit FakeLinUsbDevice(Config config = Config::reference());
    ~FakeLinUsbDevice();

    FakeLinUsbDevice(const FakeLinUsbDevice &) = delete;
    FakeLinUsbDevice &operator=(const FakeLinUsbDevice &) = delete;

    // Devices plugged in, in enumeration order.
    static QList<FakeLinUsbDevice *> present();

    // ---- host side (the test's UsbVendorInterface) ----
    bool claim();
    void release();
    Result control(bool in, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t len);
    Result bulkIn(void *data, int len, int &transferred, std::chrono::milliseconds timeout);
    Result bulkOut(const void *data, int len, std::chrono::milliseconds timeout);

    // ---- device side ----
    // lin_usb_report_frame(): false if the IN queue was full (counted as dropped)
    bool reportFrame(const lin_usb_host_frame_t &frame);
    void unplug();
    // HAL_GetTick() = offset + ms since construction
    void setTickOffset(uint32_t offset_ms);
    [[nodiscard]] uint32_t tick() const;
    // One pass over the active table of a started channel, nobody else on
    // the bus: publisher slots go out, subscriber slots get no response
    void runSchedule(uint8_t channel);
    void setBusState(uint8_t channel, uint8_t state);
    // The host controller stops polling the IN endpoint (a busy or slow host):
    // frames stay in the device until resumed
    void setHostPollsIn(bool polls);

    // LIN 2.x protected identifier: ID plus parity bits P0 (6) and P1 (7)
    [[nodiscard]] static uint8_t protectedId(uint8_t id);

    // ---- inspection ----
    [[nodiscard]] Channel channel(int ch) const;
    [[nodiscard]] std::vector<uint8_t> requestLog() const;  // bRequest of every control transfer
    [[nodiscard]] int stalls() const;
    [[nodiscard]] bool claimed() const;
    [[nodiscard]] bool outArmed() const;

private:
    // lin_usb.c, called with _mutex held
    bool inQueuePush(const lin_usb_host_frame_t &frame, bool countDrop);
    void inTrySend();
    bool reportFrameLocked(const lin_usb_host_frame_t &frame, bool countDrop);
    void outXferDone(int xferred);
    void task();
    [[nodiscard]] bool running(uint8_t ch) const;
    bool engineSetData(const lin_usb_host_frame_t &frame);
    void engineSleepWakeup(uint8_t ch, uint8_t command);
    bool controlSetup(bool in, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                      void *data, uint16_t len);

    mutable std::mutex _mutex;
    std::condition_variable _cond;
    std::chrono::steady_clock::time_point _boot;
    uint32_t _tickOffset{0};

    bool _plugged{true};
    bool _claimed{false};
    bool _hostPollsIn{true};

    // USB layer state, named as in lin_usb.c
    lin_usb_host_frame_t _outFrame{};
    bool _outArmed{true};  // armed in linusb_open() at enumeration
    lin_usb_host_frame_t _ack{};
    bool _ackPending{false};
    uint16_t _rxDropped[k_channels]{};
    lin_usb_host_frame_t _inQueue[k_inQueueSize]{};
    uint8_t _inHead{0};
    uint8_t _inTail{0};
    bool _inBusy{false};
    lin_usb_host_frame_t _inBuf{};

    // LIN engine (recording)
    Channel _channels[k_channels];
    Config _config;
    std::vector<uint8_t> _requestLog;
    int _stalls{0};
};
