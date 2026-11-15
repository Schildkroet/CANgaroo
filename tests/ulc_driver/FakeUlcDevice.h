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

// Simulated uCAN USB LIN Converter, put behind UlcInterface through its
// SerialTransportFactory.
//
// The behaviour is transcribed from the firmware, not from cangaroo:
//   https://github.com/uCAN-LIN/LinUSBConverter  embedded/Src/slcan.c, lin_slcan.c
//   (firmware 3.8, commit 5522d47)
// Function names in the comments of FakeUlcDevice.cpp point at the C code each
// part mirrors. Never adjust this file to what the driver expects: a fake that
// agrees with the driver by construction proves nothing (see issue #34).
//
// Timing model. The firmware's USB ISR (slCanProccesInput) assembles a line and
// copies it into one static command[] buffer; the main loop (slCanCheckCommand)
// answers it later. A second line that completes before that overwrites the
// first, which is then never answered. Here the main loop runs only inside the
// port's waitForReadyRead() (see tests/support/FakeSerialPort.h), so a host that
// writes a command before waiting for the previous reply loses it, and
// lostCommands() shows it.

#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

#include "support/FakeSerialPort.h"

class FakeUlcDevice final : public FakeSerialDevice
{
public:
    static constexpr int k_maxRows = 16;  // MAX_SLAVES_COUNT
    static constexpr int k_lineMax = 62;  // LINE_MAXLEN

    enum class SlcanState { Config, Open };
    enum class LinType { Master, Slave, Monitor };

    // One row of master_frame_table (t_master_frame_table_item)
    struct Row
    {
        uint8_t pid{0};
        bool transmit{false};  // OPEN_LIN_FRAME_TYPE_TRANSMIT, else RECEIVE
        uint8_t length{0};
        QByteArray data;       // data_length bytes
        uint8_t offsetMs{0};
        uint8_t responseWaitMs{0};
    };

    // A header (and data) the monitor put on the bus (transmitStd)
    struct BusTransmission
    {
        uint8_t id{0};
        bool headerOnly{false};
        QByteArray data;
    };

    FakeUlcDevice();

    // ── Test controls ────────────────────────────────────────────────────────
    void setFirmwareVersion(uint8_t major, uint8_t minor);
    void setHardwareVersion(uint8_t minor);
    // The main loop stops: commands pile up in command[] and are never answered,
    // e.g. a generic ST virtual COM port that is not a uLC.
    void setStalled(bool stalled) { _stalled = stalled; }
    // The next n commands are executed but their reply is lost.
    void dropReplies(int n) { _dropReplies = n; }

    // Frame a slave answers on the bus when the monitor sends a header for `id`.
    void setSlaveResponse(uint8_t id, const QByteArray &data);
    // A frame seen on the bus, reported to the host while the device is open
    // (lin_slcan_rx_timeout_handler only reports in SLCAN_STATE_OPEN).
    void receiveBusFrame(uint8_t id, const QByteArray &data);
    // Puts a device into the state an earlier session left it in: open as a
    // monitor, reporting `frame` on every main loop pass.
    void leaveRunningMonitor(uint8_t id, const QByteArray &data);

    // ── Inspection ───────────────────────────────────────────────────────────
    [[nodiscard]] SlcanState state() const { return _state; }
    [[nodiscard]] LinType linType() const { return _linType; }
    [[nodiscard]] uint32_t baudRate() const { return _baud; }
    [[nodiscard]] bool classicChecksum() const { return _classicChecksum; }
    [[nodiscard]] QList<Row> table() const;
    [[nodiscard]] std::optional<Row> row(uint8_t pid) const;
    // Every line the main loop executed, without CR, in order
    [[nodiscard]] const QList<QByteArray> &executedCommands() const { return _executed; }
    // Lines overwritten in command[] before the main loop got to them
    [[nodiscard]] const QList<QByteArray> &lostCommands() const { return _lost; }
    [[nodiscard]] const QList<BusTransmission> &busTransmissions() const { return _busTx; }
    // Lines that reached command[] so far (executed, lost or still pending)
    [[nodiscard]] int linesReceived() const { return _linesReceived; }

protected:
    void hostWrite(const QByteArray &data) override;
    void runMainLoop() override;

private:
    void processInput(char ch);        // slCanProccesInput
    void checkCommand(QByteArray line); // slCanCheckCommand
    bool addMasterRow(const QByteArray &line);    // addLinMasterRow
    bool transmitStd(const QByteArray &line);     // transmitStd
    [[nodiscard]] QByteArray frameLine(uint8_t pid, const QByteArray &data) const; // slcanReciveCanFrame

    uint8_t _fwMajor{3};
    uint8_t _fwMinor{8};
    uint8_t _hwMinor{1};

    SlcanState _state{SlcanState::Config};
    LinType _linType{LinType::Monitor};
    char _terminator{'\r'};
    uint32_t _baud{19200};
    bool _classicChecksum{false};
    Row _rows[k_maxRows];
    int _rowCount{0};

    QByteArray _line;
    std::optional<QByteArray> _command;
    // Frames that arrive on the bus after the current reply (one-shot echo)
    QList<QByteArray> _deferredFrames;
    std::optional<QByteArray> _chatter;
    QMap<uint8_t, QByteArray> _slaveResponses;

    bool _stalled{false};
    int _dropReplies{0};

    QList<QByteArray> _executed;
    QList<QByteArray> _lost;
    QList<BusTransmission> _busTx;
    int _linesReceived{0};
};
