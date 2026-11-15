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

// Simulated SLCAN adapters, put behind SLCANInterface through its
// SerialTransportFactory. Three firmwares, transcribed from their sources:
//
//   CANable1  github.com/normaldotcom/canable-fw   9fddea4  src/slcan.c, can.c, usbd_cdc_if.c
//   CANable2  github.com/normaldotcom/canable2-fw  b158aa7  src/slcan.c, can.c, usbd_cdc_if.c
//   WeAct     github.com/WeActStudio/WeActStudio.USB2CANFDV1.SLCAN_Firmware  d57f8c4
//             Source/App/slcan.c, Source/Bsp/can.c, Source/USB_Device/App/usbd_cdc_if.c
//
// Never adjust this to what the driver expects (see issue #34). Where the
// firmware's behaviour depends on the CAN hardware rather than on code, the
// comment says what was assumed.
//
// Differences that matter to a host:
//   - CANable sends no status reply at all (the CR/BEL lines in cdc_process are
//     commented out); WeAct answers every command with CR or BEL, and a frame
//     with CR once sent, BEL when it failed on the bus.
//   - CANable's can_enable() sets Init.Mode = NORMAL, so "M1" before "O" has no
//     effect: the channel is never silent.
//   - "S7" is 750 kbit/s on CANable (CAN_BITRATE_750K), 800 kbit/s on WeAct.
//     CANable 1 has no "S9" (83.3k); CANable 2's 750k prescaler gives 1.43 Mbit/s.
//   - CANable reports data bytes for remote frames, WeAct does not. Their
//     content is undefined (on a CANable 2.0: stale RX FIFO element bytes that
//     vary between runs); the fake reports zeros.
//   - CANable keeps its command buffer between lines: an empty line runs the
//     previous command again ("FIXME: Pressing enter repeats the previous TX").
//     On WeAct an empty line instead spoils the next command (BEL).
//
// Checked against a real CANable 2.0 (STM32G431) running canable2-fw b158aa7
// with tests/hw_compare; see the README there.

#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QList>

#include "support/FakeSerialPort.h"

class FakeSlcanDevice final : public FakeSerialDevice
{
public:
    enum class Firmware { CANable1, CANable2, WeAct };

    struct Frame
    {
        uint32_t id{0};
        bool extended{false};
        bool rtr{false};
        bool fd{false};
        bool brs{false};
        uint8_t dlc{0};   // DLC code (0-8, FD 0-F)
        QByteArray data;  // payload, empty for a remote frame
    };

    // WeAct custom timing: "Sddxxyy" / "Yddxxyy" ("Sxxyy" means div 2)
    struct CustomTiming
    {
        uint8_t div{0};
        uint8_t seg1{0};
        uint8_t seg2{0};
    };

    explicit FakeSlcanDevice(Firmware fw);

    // ── Test controls ────────────────────────────────────────────────────────
    // Frame on the bus; reported to the host while the channel is open.
    void receiveBusFrame(const Frame &frame);
    // WeAct: the next n frames fail on the bus (HAL_FDCAN_ErrorCallback, BEL).
    void failNextTransmissions(int n) { _failNext = n; }
    // State an earlier session left behind: channel open, `frame` seen on the
    // bus on every main loop pass; WeAct optionally in binary (enhanced) mode.
    // Traffic then always lands in the same pass as the next command, the
    // worst case of a race: on a CANable 2.0 at full bus load a frame slipped
    // in between clear() and "C" in 26 of 100 opens.
    void leaveRunning(const Frame &frame, bool enhancedMode = false);

    // ── Inspection ───────────────────────────────────────────────────────────
    [[nodiscard]] Firmware firmware() const { return _fw; }
    [[nodiscard]] bool onBus() const { return _onBus; }
    // Actual nominal rate; 0 while a WeAct custom timing is set
    [[nodiscard]] uint32_t nominalBitrate() const;
    [[nodiscard]] std::optional<CustomTiming> customNominal() const { return _customNominal; }
    [[nodiscard]] uint32_t dataBitrate() const;
    [[nodiscard]] std::optional<CustomTiming> customData() const { return _customData; }
    // Mode the controller runs in now (bus monitoring); only meaningful on bus
    [[nodiscard]] bool silent() const { return _onBus && _silentActive; }
    [[nodiscard]] bool enhancedMode() const { return _enhanced; }
    // error.c err_reg: sticky bit per error_t, cleared only by a reset
    [[nodiscard]] uint32_t errorRegister() const { return _errorReg; }
    [[nodiscard]] const QList<Frame> &transmitted() const { return _transmitted; }
    // Command lines in the order the firmware parsed them, without CR
    [[nodiscard]] const QList<QByteArray> &commands() const { return _commands; }
    // Commands the firmware refused (slcan_parse_str returned -1)
    [[nodiscard]] const QList<QByteArray> &rejected() const { return _rejected; }

protected:
    void hostWrite(const QByteArray &data) override;
    void runMainLoop() override;

private:
    void canableInput(char ch);  // cdc_process
    void weactInput(char ch);    // usbrxTask
    int parseCanable(int len);
    int parseWeAct(const QByteArray &line);
    // Frame command of either dialect; -1 on a malformed line. `digits` holds
    // the converted command, `len` its length.
    int parseFrameCommand(char type, const QByteArray &digits, int len);
    void transmitQueued();
    [[nodiscard]] QByteArray rxLine(const Frame &frame) const;
    [[nodiscard]] QByteArray rxEnhanced(const Frame &frame) const;

    Firmware _fw;

    bool _onBus{false};
    int _nominalIndex{4};  // CAN_BITRATE_125K, set by can_init()
    int _dataIndex{2};     // CAN_DATA_BITRATE_2M
    std::optional<CustomTiming> _customNominal;
    std::optional<CustomTiming> _customData;
    bool _silentRequested{false};
    bool _silentActive{false};
    bool _enhanced{false};

    QByteArray _input;      // host bytes not yet handled by the main loop
    QByteArray _slcanStr;   // CANable: slcan_str[SLCAN_MTU], kept between lines
    int _strIndex{0};       // CANable: slcan_str_index
    QByteArray _weactData;  // WeAct: usbrxTask data[], empty = PARSE_IDLE
    QList<Frame> _txQueue;
    std::optional<Frame> _chatter;
    int _failNext{0};
    uint32_t _errorReg{0};


    QList<Frame> _transmitted;
    QList<QByteArray> _commands;
    QList<QByteArray> _rejected;
};
