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

#include "FakeUlcDevice.h"

namespace
{

constexpr char k_bell = 0x07;
constexpr char k_cr = '\r';
constexpr char k_lf = '\n';

// Byte at `i`, 0 past the end: the firmware's line is NUL terminated.
char at(const QByteArray &line, int i)
{
    return i < line.size() ? line.at(i) : '\0';
}

// parseHex() in slcan.c: fails on NUL or a non-hex digit
bool parseHex(const QByteArray &line, int pos, int len, uint32_t &value)
{
    value = 0;
    for (int i = 0; i < len; ++i)
    {
        const char c = at(line, pos + i);
        value <<= 4;
        if (c >= '0' && c <= '9')
            value += c - '0';
        else if (c >= 'A' && c <= 'F')
            value += c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            value += c - 'a' + 10;
        else
            return false;
    }
    return true;
}

void appendHex(QByteArray &out, uint8_t v)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    out.append(digits[v >> 4]);
    out.append(digits[v & 0x0F]);
}

} // namespace


// ── Device ───────────────────────────────────────────────────────────────────

FakeUlcDevice::FakeUlcDevice() = default;

void FakeUlcDevice::setFirmwareVersion(uint8_t major, uint8_t minor)
{
    _fwMajor = major;
    _fwMinor = minor;
}

void FakeUlcDevice::setHardwareVersion(uint8_t minor)
{
    _hwMinor = minor;
}

void FakeUlcDevice::setSlaveResponse(uint8_t id, const QByteArray &data)
{
    _slaveResponses.insert(id, data);
}

void FakeUlcDevice::receiveBusFrame(uint8_t id, const QByteArray &data)
{
    if (_state == SlcanState::Open)
        _output += frameLine(id, data);
}

void FakeUlcDevice::leaveRunningMonitor(uint8_t id, const QByteArray &data)
{
    _state = SlcanState::Open;
    _linType = LinType::Monitor;
    _chatter = frameLine(id, data);
}

QList<FakeUlcDevice::Row> FakeUlcDevice::table() const
{
    return QList<Row>(std::begin(_rows), std::begin(_rows) + _rowCount);
}

std::optional<FakeUlcDevice::Row> FakeUlcDevice::row(uint8_t pid) const
{
    for (int i = 0; i < _rowCount; ++i)
    {
        if (_rows[i].pid == pid)
            return _rows[i];
    }
    return std::nullopt;
}

void FakeUlcDevice::hostWrite(const QByteArray &data)
{
    // CDC_Receive_FS feeds every byte to slCanProccesInput
    for (char ch : data)
        processInput(ch);
}

// slCanProccesInput: runs in the USB ISR, a finished line replaces command[]
void FakeUlcDevice::processInput(char ch)
{
    if (ch == k_cr)
    {
        if (_command)
            _lost.append(*_command);
        _command = _line;
        _line.clear();
        ++_linesReceived;
    }
    else if (ch != k_lf)
    {
        // linepos stops at LINE_MAXLEN - 1: further bytes overwrite the last one
        if (_line.size() < k_lineMax - 1)
            _line.append(ch);
        else
            _line[_line.size() - 1] = ch;
    }
}

void FakeUlcDevice::runMainLoop()
{
    if (_stalled)
        return;

    // Bus traffic of the running session (main loop and UART ISR interleave)
    if (_chatter && _state == SlcanState::Open)
        _output += *_chatter;

    if (_command)
    {
        const QByteArray line = *_command;
        _command.reset();
        checkCommand(line);
    }

    // The idle timeout after the reply reports what came back from the bus.
    for (const QByteArray &frame : std::as_const(_deferredFrames))
        _output += frame;
    _deferredFrames.clear();
}

// slCanCheckCommand
void FakeUlcDevice::checkCommand(QByteArray line)
{
    if (line.isEmpty())
        return;  // case 0: no reply at all

    _executed.append(line);

    char result = k_bell;
    QByteArray reply;

    switch (line.at(0))
    {
    case 'a':
        _terminator = (_terminator == k_cr) ? k_lf : k_cr;
        result = _terminator;
        break;

    case 'S':
    case 'G':
    case 'W':
    case 's':
        _baud = (at(line, 1) == '2') ? 9600 : 19200;
        result = _terminator;
        break;

    case 'F':
        result = _terminator;
        break;

    case 'V':
        reply.append('V');
        appendHex(reply, _hwMinor);
        result = _terminator;
        break;

    case 'v':
        reply.append('v');
        appendHex(reply, _fwMajor);
        appendHex(reply, _fwMinor);
        result = _terminator;
        break;

    case 'N':
        reply.append('N');
        for (int i = 0; i < 4; ++i)
            appendHex(reply, 0);
        result = _terminator;
        break;

    case 'o':
    case 'O':
        if (_state == SlcanState::Config)
        {
            _linType = LinType::Master;
            result = _terminator;
        }
        break;

    case 'L':
        if (_state == SlcanState::Config)
        {
            result = _terminator;
            _linType = LinType::Slave;
            _state = SlcanState::Open;
        }
        break;

    case 'l':
        if (_state == SlcanState::Config)
        {
            result = _terminator;
            _linType = LinType::Monitor;
            _state = SlcanState::Open;
        }
        break;

    case 'C':
        _state = SlcanState::Config;
        result = _terminator;
        _linType = LinType::Master;  // sic: close selects master
        _chatter.reset();
        break;

    case 'R':
    case 'r':
    case 'T':
    case 't':
    {
        const bool upper = line.at(0) < 'Z';

        if (at(line, 1) == '3')  // checksum mode
        {
            _classicChecksum = at(line, 2) == '1';
            reply.append('Z');
            result = _terminator;
            break;
        }

        if (at(line, 2) == '4')  // baud rate, firmware 3.x
        {
            uint32_t baud = 0;
            if (!parseHex(line, 3, 4, baud) || baud == 0)
                break;
            _baud = baud;
            reply.append('Z');
            result = _terminator;
            break;
        }

        switch (_linType)
        {
        case LinType::Master:
        case LinType::Slave:
            if (addMasterRow(line))
                reply.append(upper ? 'Z' : 'z');
            result = _terminator;  // a bare CR when the row was not taken
            break;
        case LinType::Monitor:
            if (_state == SlcanState::Open && transmitStd(line))
            {
                reply.append(upper ? 'Z' : 'z');
                result = _terminator;
            }
            break;
        }
        break;
    }

    default:
        break;
    }

    reply.append(result);

    if (_dropReplies > 0)
    {
        --_dropReplies;
        _deferredFrames.clear();
        return;
    }
    _output += reply;
}

// addLinMasterRow (lin_slcan.c). Fields are written into the row as they are
// parsed, so a malformed command can leave a row half updated, as on the device.
bool FakeUlcDevice::addMasterRow(const QByteArray &line)
{
    const bool upper = line.at(0) < 'Z';
    const int offset = upper ? 5 : 0;
    uint32_t temp = 0;

    if (at(line, 1) == '2')  // reset schedule table
    {
        _state = SlcanState::Config;
        _rowCount = 0;
        return true;
    }
    if (at(line, 1) == '1')  // start
    {
        _state = SlcanState::Open;
        return true;
    }

    if (!parseHex(line, 2, 2, temp))
        return false;

    // slcan_get_master_table_row: the row with this PID, else the next free one
    int index = 0;
    while (index < _rowCount && _rows[index].pid != temp)
        ++index;
    if (index >= k_maxRows)
        return false;  // table full

    Row &row = _rows[index];
    row.pid = static_cast<uint8_t>(temp);

    if (!parseHex(line, 4 + offset, 1, temp))
        return false;
    if (temp > 8)
        return false;
    row.length = static_cast<uint8_t>(temp);
    row.data.resize(row.length);

    const char type = line.at(0);
    row.transmit = !(type == 'r' || type == 'R');

    if (upper)
    {
        if (!parseHex(line, 4, 2, temp))
            return false;
        row.offsetMs = static_cast<uint8_t>(temp);
        if (!parseHex(line, 6, 2, temp))
            return false;
        row.responseWaitMs = static_cast<uint8_t>(temp);
    }
    else
    {
        // The untimed form resets the row to the firmware's default timing.
        row.offsetMs = 15;
        const uint32_t frameMaxMs = ((row.length * 10u + 44u) * 1400u) / _baud + 1u;
        row.responseWaitMs = static_cast<uint8_t>(frameMaxMs);
    }

    if (row.transmit)
    {
        for (int i = 0; i < row.length; ++i)
        {
            if (!parseHex(line, 5 + offset + i * 2, 2, temp))
                return false;
            row.data[i] = static_cast<char>(temp);
        }
    }

    if (index == _rowCount && _rowCount < k_maxRows)
        ++_rowCount;
    return true;
}

// transmitStd (slcan.c): header and data go out with blocking UART writes,
// then lin_slcan_skip_header_reception() restarts reception. The node's own
// data bytes are over by then; only a slave's answer to a header is collected.
// The idle timeout reports what was collected, an empty frame if nothing was.
bool FakeUlcDevice::transmitStd(const QByteArray &line)
{
    const bool upper = line.at(0) < 'Z';
    const int offset = upper ? 5 : 0;
    const bool withData = line.at(0) == 't' || line.at(0) == 'T';
    uint32_t temp = 0;

    if (!parseHex(line, 2 + offset, 2, temp))
        return false;
    const uint8_t id = static_cast<uint8_t>(temp & 0x3F);

    if (!parseHex(line, 4 + offset, 1, temp))
        return false;
    const int length = static_cast<int>(temp);
    if (length > 8)
        return false;

    QByteArray data;
    if (withData)
    {
        for (int i = 0; i < length; ++i)
        {
            if (!parseHex(line, 5 + offset + i * 2, 2, temp))
                return false;
            data.append(static_cast<char>(temp));
        }
    }

    _busTx.append(BusTransmission{.id = id, .headerOnly = !withData, .data = data});

    // Reported after the command's reply, see runMainLoop()
    _deferredFrames.append(frameLine(id, withData ? QByteArray() : _slaveResponses.value(id)));
    return true;
}

// slcanReciveCanFrame
QByteArray FakeUlcDevice::frameLine(uint8_t pid, const QByteArray &data) const
{
    QByteArray frame;
    frame.append('t');
    frame.append('0');  // "for slcan compatibility"
    appendHex(frame, pid & 0x3F);
    frame.append(static_cast<char>('0' + data.size()));
    for (char b : data)
        appendHex(frame, static_cast<uint8_t>(b));
    frame.append(_terminator);
    return frame;
}
