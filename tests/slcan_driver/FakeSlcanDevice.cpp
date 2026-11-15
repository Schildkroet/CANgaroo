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

#include "FakeSlcanDevice.h"

#include <array>

namespace
{

constexpr char k_cr = '\r';
constexpr char k_bell = 0x07;

// slcan.h: SLCAN_OK / SLCAN_ERROR / SLCAN_OK_NOT_RESPONSE
constexpr int k_ok = 0;
constexpr int k_error = -1;
constexpr int k_okNoResponse = 1;

// hal_dlc_code_to_bytes
constexpr std::array<uint8_t, 16> k_dlcToBytes = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

// CANable 1, can_set_bitrate(): 48 MHz, 1 + 4 + 3 time quanta
constexpr std::array<uint32_t, 9> k_canable1Prescaler = {600, 300, 120, 60, 48, 24, 12, 8, 6};
// CANable 2, can_set_bitrate(): 170 MHz, 1 + 14 + 2 time quanta. Index order
// is enum can_bitrate: ..., 750K (prescaler 7, "VERY INACCURATE"), 1000K, 83.3K.
constexpr std::array<uint32_t, 10> k_canable2Prescaler = {1000, 500, 200, 100, 80, 40, 20, 7, 10, 120};
// WeAct, enum can_bitrate and the protocol table in the WeAct README
constexpr std::array<uint32_t, 14> k_weactBitrate = {10000, 20000, 50000, 100000, 125000, 250000, 500000,
                                                     800000, 1000000, 83333, 75000, 62500, 33333, 5000};

uint32_t canable2Rate(uint32_t prescaler)
{
    return 170000000u / (prescaler * 17u);
}

// The ASCII-to-nibble step at the top of slcan_parse_str. CANable converts
// without checking; WeAct refuses anything that is not a hex digit.
bool toDigits(const QByteArray &line, bool strict, QByteArray &digits)
{
    digits = line;
    for (int i = 1; i < line.size(); ++i)
    {
        const char c = line.at(i);
        if (c >= 'a')
        {
            if (strict && c > 'f')
                return false;
            digits[i] = static_cast<char>(c - 'a' + 10);
        }
        else if (c >= 'A')
        {
            if (strict && c > 'F')
                return false;
            digits[i] = static_cast<char>(c - 'A' + 10);
        }
        else
        {
            if (strict && c > '9')
                return false;
            digits[i] = static_cast<char>(c - '0');
        }
    }
    return true;
}

// Digit at `i`; 0 past the buffer
uint8_t digit(const QByteArray &digits, int i)
{
    return i < digits.size() ? static_cast<uint8_t>(digits.at(i)) : 0;
}

void appendHex(QByteArray &out, uint32_t value, int digits)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; --i)
        out.append(hex[(value >> (4 * i)) & 0xF]);
}

} // namespace


FakeSlcanDevice::FakeSlcanDevice(Firmware fw)
    : _fw(fw)
    // SLCAN_MTU: canable-fw 30, canable2-fw 138 + 1 + 16
    , _slcanStr(fw == Firmware::CANable1 ? 30 : 155, '\0')
{
}

uint32_t FakeSlcanDevice::nominalBitrate() const
{
    switch (_fw)
    {
    case Firmware::CANable1:
        return 48000000u / (k_canable1Prescaler.at(_nominalIndex) * 8u);
    case Firmware::CANable2:
        return canable2Rate(k_canable2Prescaler.at(_nominalIndex));
    case Firmware::WeAct:
        return _customNominal ? 0 : k_weactBitrate.at(_nominalIndex);
    }
    return 0;
}

uint32_t FakeSlcanDevice::dataBitrate() const
{
    switch (_fw)
    {
    case Firmware::CANable1:
        return 0;
    case Firmware::CANable2:
        // can_set_data_bitrate(): data_prescaler 5 (2M) or 2 (5M), 17 tq
        return canable2Rate(_dataIndex == 5 ? 2 : 5);
    case Firmware::WeAct:
        return _customData ? 0 : static_cast<uint32_t>(_dataIndex) * 1000000u;
    }
    return 0;
}

void FakeSlcanDevice::receiveBusFrame(const Frame &frame)
{
    if (!_onBus)
        return;
    // bxCAN (CANable 1) cannot receive FD frames; the bus sees error frames.
    if (frame.fd && _fw == Firmware::CANable1)
        return;

    _output += (_enhanced ? rxEnhanced(frame) : rxLine(frame));
}

void FakeSlcanDevice::leaveRunning(const Frame &frame, bool enhancedMode)
{
    _onBus = true;
    _silentActive = false;
    _enhanced = enhancedMode && _fw == Firmware::WeAct;
    _chatter = frame;
}

void FakeSlcanDevice::hostWrite(const QByteArray &data)
{
    _input += data;
}

void FakeSlcanDevice::runMainLoop()
{
    // Traffic on the bus while the host was busy
    if (_chatter && _onBus)
        receiveBusFrame(*_chatter);

    for (char ch : std::as_const(_input))
    {
        if (_fw == Firmware::WeAct)
            weactInput(ch);
        else
            canableInput(ch);
    }
    _input.clear();

    transmitQueued();
}

// cdc_process: CR runs slcan_parse_str on slcan_str, which is not cleared. A
// shorter line leaves the tail of the previous one behind, and an empty line
// runs slcan_str[0] again. The result is not reported (the CR/BEL lines are
// commented out).
void FakeSlcanDevice::canableInput(char ch)
{
    if (ch == k_cr)
    {
        const QByteArray raw = _slcanStr.left(_strIndex);
        _commands.append(raw);
        if (parseCanable(_strIndex) == k_error)
            _rejected.append(raw);
        _strIndex = 0;
        return;
    }
    if (_strIndex >= _slcanStr.size())
        _strIndex = 0;  // overflow: start over
    _slcanStr[_strIndex++] = ch;
}

// usbrxTask, PARSE_IDLE / PARSE_SLCAN (binary mode input is not modelled; the
// driver only sends ASCII). The first byte is stored whatever it is, so a lone
// CR becomes the start of the next command. SLCAN_OK -> CR, SLCAN_ERROR -> BEL,
// OK_NOT_RESPONSE -> nothing.
void FakeSlcanDevice::weactInput(char ch)
{
    constexpr int k_mtu = 1 + 8 + 1 + 128 + 1;  // SLCAN_MTU

    if (_weactData.isEmpty())
    {
        _weactData.append(ch);
        return;
    }
    if (ch != k_cr)
    {
        if (_weactData.size() >= k_mtu)
            _weactData.clear();  // state = PARSE_IDLE
        else
            _weactData.append(ch);
        return;
    }

    const QByteArray line = _weactData;
    _weactData.clear();
    _commands.append(line);
    const int rc = parseWeAct(line);
    if (rc == k_error)
    {
        _rejected.append(line);
        _output += k_bell;
    }
    else if (rc == k_ok)
    {
        _output += k_cr;
    }
}

int FakeSlcanDevice::parseCanable(int len)
{
    const bool canable2 = _fw == Firmware::CANable2;
    // In place, as the firmware does: only the new line's bytes are converted,
    // the rest of slcan_str keeps what earlier lines left there.
    QByteArray &digits = _slcanStr;
    QByteArray converted;
    toDigits(digits.left(len), false, converted);
    digits.replace(0, len, converted);

    switch (digits.at(0))
    {
    case 'O':
        if (!_onBus)
        {
            _onBus = true;
            _silentActive = false;  // can_enable(): Init.Mode = NORMAL
        }
        return k_ok;
    case 'C':
        _onBus = false;
        return k_ok;
    case 'S':
    {
        const int invalid = canable2 ? 10 : 9;  // CAN_BITRATE_INVALID
        if (digit(digits, 1) >= invalid)
            return k_error;
        if (!_onBus)
            _nominalIndex = digit(digits, 1);
        return k_ok;
    }
    case 'Y':
        if (!canable2)
            return k_error;
        if (digit(digits, 1) != 2 && digit(digits, 1) != 5)
            return k_error;
        _dataIndex = digit(digits, 1);
        return k_ok;
    case 'm':
    case 'a':
        if (canable2)
            return k_error;
        [[fallthrough]];
    case 'M':
    case 'A':
        if (digits.at(0) == 'M' || digits.at(0) == 'm')
        {
            if (!_onBus)
                _silentRequested = digit(digits, 1) == 1;  // can_set_silent(), overridden by can_enable()
        }
        return k_ok;
    case 'V':
        // GIT_VERSION " " GIT_REMOTE, the remote cut to "github..." by the Makefile
        _output += canable2 ? QByteArrayLiteral("b158aa7 github.com/normaldotcom/canable2-fw.git\r")
                            : QByteArrayLiteral("9fddea4 github.com/normaldotcom/canable-fw.git\r");
        return k_ok;
    case 'E':
        _output += "CANable Error Register: " + QByteArray::number(_errorReg, 16).toUpper();
        return k_ok;
    case 't':
    case 'T':
    case 'r':
    case 'R':
        return parseFrameCommand(digits.at(0), digits, len);
    case 'd':
    case 'D':
    case 'b':
    case 'B':
        return canable2 ? parseFrameCommand(digits.at(0), digits, len) : k_error;
    default:
        return k_error;
    }
}

int FakeSlcanDevice::parseWeAct(const QByteArray &line)
{
    QByteArray digits;
    if (!toDigits(line, true, digits))
        return k_error;

    const int len = static_cast<int>(line.size());
    const auto byteAt = [&](int i) { return static_cast<uint8_t>(digit(digits, i) << 4 | digit(digits, i + 1)); };

    switch (line.at(0))
    {
    case 'O':
        if (len != 1)
            return k_error;
        if (!_onBus)
        {
            _onBus = true;
            _silentActive = _silentRequested;  // can_enable(): Init.Mode = can_mode
        }
        return k_ok;
    case 'C':
        if (len != 1)
            return k_error;
        _onBus = false;
        return k_ok;
    case 'S':
        if (len == 2)
        {
            if (digit(digits, 1) >= k_weactBitrate.size())
                return k_error;
            if (!_onBus)  // can_set_bitrate() returns early on bus, still SLCAN_OK
            {
                _nominalIndex = digit(digits, 1);
                _customNominal.reset();
            }
            return k_ok;
        }
        if (len == 5 || len == 7)
        {
            // can_set_bitrate2 / can_set_bitrate3
            const CustomTiming t = (len == 5) ? CustomTiming{2, byteAt(1), byteAt(3)}
                                              : CustomTiming{byteAt(1), byteAt(3), byteAt(5)};
            if (_onBus || t.div == 0 || t.seg1 < 2 || t.seg2 < 2 || t.seg2 > 128)
                return k_error;
            _customNominal = t;
            return k_ok;
        }
        return k_error;
    case 'Y':
        if (len == 2)
        {
            if (digit(digits, 1) >= 6)  // CAN_DATA_BITRATE_INVALID
                return k_error;
            if (!_onBus)
            {
                _dataIndex = digit(digits, 1);
                _customData.reset();
            }
            return k_ok;
        }
        if (len == 5 || len == 7)
        {
            // can_set_data_bitrate2 / can_set_data_bitrate3
            const CustomTiming t = (len == 5) ? CustomTiming{1, byteAt(1), byteAt(3)}
                                              : CustomTiming{byteAt(1), byteAt(3), byteAt(5)};
            if (_onBus || t.div == 0 || t.seg1 == 0 || t.seg2 == 0 || t.div > 32 || t.seg1 > 32 || t.seg2 > 16)
                return k_error;
            _customData = t;
            return k_ok;
        }
        return k_error;
    case 'M':
        if (len != 2 || _onBus || digit(digits, 1) > 1)
            return k_error;
        _silentRequested = digit(digits, 1) == 1;
        return k_ok;
    case 'A':
        if (len != 2 || _onBus || digit(digits, 1) > 1)
            return k_error;
        return k_ok;
    case 'H':
        if (len != 2 || digit(digits, 1) > 1)
            return k_error;
        _enhanced = digit(digits, 1) == 1;
        return k_ok;
    case 'f':
        return (len == 7 && !_onBus) ? k_ok : k_error;
    case 'F':
        return (len == 17 && !_onBus) ? k_ok : k_error;
    case 'V':
        if (len != 1)
            return k_error;
        // bootloader_get_version() lives in the closed bootloader; the format
        // follows the fw_id string left commented out in slcan.c.
        _output += QByteArrayLiteral("WeAct Studio 2024.01.10 V1.0.0.6\r");
        return k_okNoResponse;
    case 'E':
        if (len != 1)
            return k_error;
        _output += "CANable Error Register: " + QByteArray::number(_errorReg, 16).toUpper();
        return k_okNoResponse;
    case 'X':
        return (len == 1 && !_onBus) ? k_ok : k_error;
    case 't':
    case 'T':
    case 'r':
    case 'R':
    case 'd':
    case 'D':
    case 'b':
    case 'B':
        if (!_onBus)
            return k_error;
        return parseFrameCommand(line.at(0), digits, len);
    default:
        return k_error;
    }
}

// Frame part of slcan_parse_str, both dialects
int FakeSlcanDevice::parseFrameCommand(char type, const QByteArray &digits, int len)
{
    Frame frame;
    frame.extended = type >= 'A' && type <= 'Z';
    const char lower = static_cast<char>(frame.extended ? type + 32 : type);
    frame.rtr = lower == 'r';
    frame.fd = lower == 'd' || lower == 'b';
    frame.brs = lower == 'b';

    const int idLen = frame.extended ? 8 : 3;  // SLCAN_EXT_ID_LEN / SLCAN_STD_ID_LEN
    int pos = 1;
    for (; pos <= idLen; ++pos)
        frame.id = (frame.id << 4) | digit(digits, pos);

    frame.dlc = digit(digits, pos++);
    if (frame.dlc > (frame.fd ? 0xF : 0x8))
        return k_error;
    const int bytes = k_dlcToBytes.at(frame.dlc);

    if (_fw == Firmware::WeAct)
    {
        // "Check if len != command + id + length + bytes_in_msg"
        const int expected = 1 + idLen + 1 + (frame.rtr ? 0 : bytes * 2);
        if (len != expected)
            return k_error;
    }

    if (!frame.rtr)
    {
        for (int i = 0; i < bytes; ++i, pos += 2)
            frame.data.append(static_cast<char>(digit(digits, pos) << 4 | digit(digits, pos + 1)));
    }

    _txQueue.append(frame);
    return k_okNoResponse;  // WeAct: can_tx() queues, the reply follows the transmission
}

// can_process / cantxTask and the HAL TX callbacks
void FakeSlcanDevice::transmitQueued()
{
    for (const Frame &frame : std::as_const(_txQueue))
    {
        if (!_onBus)
        {
            // can_process: HAL_FDCAN_AddMessageToTxFifoQ fails on a stopped
            // controller, the frame is dropped and ERR_CAN_TXFAIL (bit 2) set.
            // Seen on a CANable 2.0 (error register 4). CANable 1's bxCAN HAL
            // may accept the frame instead; not checked, so only dropped there.
            if (_fw == Firmware::CANable2)
                _errorReg |= 1u << 2;
            continue;
        }

        if (_fw != Firmware::WeAct)
        {
            _transmitted.append(frame);  // CANable reports nothing either way
            continue;
        }

        // Assumption (FDCAN reference manual): in bus monitoring mode the
        // controller does not start a transmission, so no callback ever fires.
        if (_silentActive)
            continue;

        if (_failNext > 0)
        {
            --_failNext;
            _output += k_bell;  // HAL_FDCAN_ErrorCallback
            continue;
        }
        _transmitted.append(frame);
        _output += k_cr;  // HAL_FDCAN_TxBufferCompleteCallback
    }
    _txQueue.clear();
}

// slcan_parse_frame
QByteArray FakeSlcanDevice::rxLine(const Frame &frame) const
{
    char type = frame.fd ? (frame.brs ? 'b' : 'd') : (frame.rtr ? 'r' : 't');
    if (frame.extended)
        type = static_cast<char>(type - 32);

    QByteArray line(1, type);
    appendHex(line, frame.id, frame.extended ? 8 : 3);
    appendHex(line, frame.dlc, 1);

    const int bytes = k_dlcToBytes.at(frame.dlc);
    if (frame.rtr)
    {
        // CANable copies the (unwritten) data area of a remote frame too, with
        // undefined content; WeAct leaves it out since V1.0.0.6.
        if (_fw != Firmware::WeAct)
            for (int i = 0; i < bytes; ++i)
                appendHex(line, 0, 2);
    }
    else
    {
        for (int i = 0; i < bytes; ++i)
            appendHex(line, i < frame.data.size() ? static_cast<uint8_t>(frame.data.at(i)) : 0, 2);
    }
    line.append(k_cr);
    return line;
}

// slcan_eh_parse_frame (WeAct binary mode): header, length, little-endian ID,
// DLC code, data; length counts ID, DLC and data.
QByteArray FakeSlcanDevice::rxEnhanced(const Frame &frame) const
{
    char type = frame.fd ? (frame.brs ? 'b' : 'd') : (frame.rtr ? 'r' : 't');
    if (frame.extended)
        type = static_cast<char>(type - 32);

    const int idBytes = frame.extended ? 4 : 2;
    const int bytes = frame.rtr ? 0 : k_dlcToBytes.at(frame.dlc);

    QByteArray msg;
    msg.append(static_cast<char>(0x80 + type));
    msg.append(static_cast<char>(idBytes + 1 + bytes));
    for (int i = 0; i < idBytes; ++i)
        msg.append(static_cast<char>((frame.id >> (8 * i)) & 0xFF));
    msg.append(static_cast<char>(frame.dlc));
    msg.append(frame.data.left(bytes));
    return msg;
}
