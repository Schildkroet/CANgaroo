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

#include <algorithm>
#include <array>
#include <cmath>

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
// WeAct, enum can_bitrate and the protocol table in the WeAct README. Elmue
// uses the same letters (Slcan/control.h, CAN_NOM_BAUDRATE_*).
constexpr std::array<uint32_t, 14> k_weactBitrate = {10000, 20000, 50000, 100000, 125000, 250000, 500000,
                                                     800000, 1000000, 83333, 75000, 62500, 33333, 5000};

// Elmue, Slcan/control.c for STM32G4 (160 MHz CAN clock): {BRP, Seg1, Seg2}
// per "S" letter (CAN_NOM_BITTIMING_*, indexed like k_weactBitrate) and per
// "Y" digit (CAN_DATA_BITTIMING_*). SJW: control_set_baudrate().
constexpr uint32_t k_elmueClock = 160000000;
constexpr std::array<std::array<uint32_t, 3>, 14> k_elmueNominal = {{
    {50, 239, 80}, {25, 239, 80}, {10, 239, 80}, {5, 239, 80}, {4, 239, 80}, {2, 239, 80}, {1, 239, 80},
    {1, 149, 50}, {1, 119, 40}, {6, 239, 80}, {9, 176, 60}, {8, 239, 80}, {15, 239, 80}, {100, 239, 80},
}};
constexpr std::array<uint32_t, 3> elmueData(char y)
{
    switch (y)
    {
    case '0': return {8, 29, 10};
    case '1': return {4, 29, 10};
    case '2': return {2, 29, 10};
    case '4': return {2, 14, 5};
    case '5': return {2, 11, 4};
    case '8': return {2, 4, 5};
    default:  return {0, 0, 0};
    }
}

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
int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

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
    // SLCAN_MTU: canable-fw 30, canable2-fw 138 + 1 + 16, Elmue 1 + 138 + 8 + 1 + 1 + 16
    , _slcanStr(fw == Firmware::CANable1 ? 30 : (fw == Firmware::Elmue ? 165 : 155), '\0')
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
    case Firmware::Elmue:
        if (!_elmueNominal)
            return 0;
        return static_cast<uint32_t>(std::lround(double(k_elmueClock) /
                                                 (_elmueNominal->brp * (1.0 + _elmueNominal->seg1 + _elmueNominal->seg2))));
    }
    return 0;
}

uint32_t FakeSlcanDevice::samplePoint(bool data) const
{
    const std::optional<ElmueTiming> &t = data ? _elmueData : _elmueNominal;
    if (_fw != Firmware::Elmue || !t)
        return 0;
    const uint32_t quanta = 1 + t->seg1 + t->seg2;
    return ((1 + t->seg1) * 1000 + quanta / 2) / quanta;
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
    case Firmware::Elmue:
        if (!_elmueData)
            return 0;
        return static_cast<uint32_t>(std::lround(double(k_elmueClock) /
                                                 (_elmueData->brp * (1.0 + _elmueData->seg1 + _elmueData->seg2))));
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
    if (_fw == Firmware::Elmue && !_elmueNominal)
        _elmueNominal = ElmueTiming{1, 239, 80, 79};  // an open channel has a bitrate: S6
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
        else if (_fw == Firmware::Elmue)
            elmueInput(ch);
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

// buf_process: like CANable, but control_parse_command() terminates the line,
// so nothing of the previous one is reused; an empty line is a success.
void FakeSlcanDevice::elmueInput(char ch)
{
    if (ch == k_cr)
    {
        const QByteArray line = _slcanStr.left(_strIndex);
        _strIndex = 0;
        _commands.append(line);
        parseElmue(line);
        return;
    }
    if (_strIndex >= _slcanStr.size())
        _strIndex = 0;  // APP_UsbInOverflow
    _slcanStr[_strIndex++] = ch;
}

void FakeSlcanDevice::parseElmue(const QByteArray &line)
{
    const auto fail = [&] {
        _rejected.append(line);
        _output += k_bell;
    };
    const auto ok = [&] { _output += k_cr; };
    const int len = static_cast<int>(line.size());

    if (len == 0)
        return ok();

    const char cmd = line.at(0);
    switch (cmd)
    {
    case 'A':
        if (len != 2 || _onBus || (line.at(1) != '0' && line.at(1) != '1'))
            return fail();
        return ok();
    case 'M':
    {
        if (len < 2)
            return fail();
        for (int i = 1; i < len; ++i)
        {
            const char m = line.at(i);
            if ((m == '0' || m == '1') && !_onBus)
                _silentRequested = m == '1';  // FDCAN_MODE_BUS_MONITORING for the next Open
            else
                return fail();  // open, or a mode the fake does not model
        }
        return ok();
    }
    case 'O':
        if (len > 2)
            return fail();
        if (len == 2)
        {
            if (line.at(1) == 'N')
                _silentRequested = false;
            else if (line.at(1) == 'S')
                _silentRequested = true;
            else
                return fail();  // loopback modes not modelled
        }
        // can_open(): not twice, and not without a nominal baudrate
        if (_onBus || !_elmueNominal)
            return fail();
        _onBus = true;
        _silentActive = _silentRequested;
        return ok();
    case 'C':
        if (len != 1)
            return fail();
        if (_onBus)
        {
            // can_close() -> can_reset(): bitrates are forgotten
            _onBus = false;
            _elmueNominal.reset();
            _elmueData.reset();
        }
        _silentRequested = false;  // CanMode = NORMAL
        return;                    // never answers
    case 'V':
        if (len != 1)
            return fail();
        // Board, MCU, DevID, clock, limits, HAL and serial as a CANable 2.0 nano
        // (G431 Multiboard build) reports them; Firmware and Slcan from settings.h.
        _output += QByteArrayLiteral(
            "+Board: Multiboard\tMCU: STM32G431\tDevID: 1128\tFirmware: 2494465\tSlcan: 105"
            "\tClock: 160\tChannels: 1\tQuartz: No\tLimits: 512,256,128,128,32,32,16,16"
            "\tHAL: 1.2.5\tSerial: 206E38A550565008\r");
        return;
    case 'S':
    case 'Y':
    {
        if (len != 2)
            return fail();
        const char b = line.at(1);
        std::array<uint32_t, 3> t{0, 0, 0};
        if (cmd == 'S')
        {
            if (b >= '0' && b <= '9')
                t = k_elmueNominal.at(b - '0');
            else if (b >= 'A' && b <= 'D')
                t = k_elmueNominal.at(b - 'A' + 10);
        }
        else
        {
            t = elmueData(b);
        }
        if (t[0] == 0)
            return fail();  // undefined letter
        // control_set_baudrate(): SJW = min(seg1, seg2, max) - 1
        uint32_t sjw = std::min({t[1], t[2], cmd == 'S' ? 128u : 16u});
        if (sjw > 1)
            --sjw;
        return elmueSetTiming(cmd == 'Y', ElmueTiming{t[0], t[1], t[2], sjw}) ? ok() : fail();
    }
    case 's':
    case 'y':
    {
        // "sBRP,Seg1,Seg2,Sjw": utils_parse_next_decimal, separators ',' then end
        const QList<QByteArray> parts = line.mid(1).split(',');
        if (parts.size() != 4)
            return fail();
        uint32_t v[4];
        for (int i = 0; i < 4; ++i)
        {
            bool okDigits = !parts[i].isEmpty();
            for (char c : parts[i])
                okDigits = okDigits && c >= '0' && c <= '9';
            if (!okDigits)
                return fail();
            v[i] = parts[i].toUInt();
        }
        return elmueSetTiming(cmd == 'y', ElmueTiming{v[0], v[1], v[2], v[3]}) ? ok() : fail();
    }
    case 't':
    case 'T':
    case 'r':
    case 'R':
    case 'd':
    case 'D':
    case 'b':
    case 'B':
        return elmueFrame(line) == k_ok ? ok() : fail();
    default:
        return fail();  // FBK_InvalidCommand, or an advanced command not modelled
    }
}

// can_set_bit_timing(): only while closed, within the HAL's IS_FDCAN_* ranges,
// and not above MAX_CAN_BAUDRATE (10 MBaud)
bool FakeSlcanDevice::elmueSetTiming(bool data, const ElmueTiming &t)
{
    if (_onBus)
        return false;
    const bool inRange = data
        ? (t.brp >= 1 && t.brp <= 32 && t.seg1 >= 1 && t.seg1 <= 32 && t.seg2 >= 1 && t.seg2 <= 16 && t.sjw >= 1 && t.sjw <= 16)
        : (t.brp >= 1 && t.brp <= 512 && t.seg1 >= 1 && t.seg1 <= 256 && t.seg2 >= 1 && t.seg2 <= 128 && t.sjw >= 1 && t.sjw <= 128);
    if (!inRange)
        return false;
    if (double(k_elmueClock) / (t.brp * (1.0 + t.seg1 + t.seg2)) > 10e6)
        return false;
    (data ? _elmueData : _elmueNominal) = t;
    return true;
}

// Transmit part of control_parse_str and buf_store_tx_packet
int FakeSlcanDevice::elmueFrame(const QByteArray &line)
{
    const char type = line.at(0);
    Frame frame;
    frame.extended = type >= 'A' && type <= 'Z';
    const char lower = static_cast<char>(frame.extended ? type + 32 : type);
    frame.rtr = lower == 'r';
    frame.fd = lower == 'd' || lower == 'b';
    frame.brs = lower == 'b';

    // Sending with FDF needs a data baudrate (FBK_BaudrateNotSet)
    if (frame.fd && !_elmueData)
        return k_error;

    // utils_parse_hex_value: exactly n hex digits
    int pos = 1;
    const auto hex = [&](int digits, uint32_t &value) {
        value = 0;
        for (int i = 0; i < digits; ++i, ++pos)
        {
            const int n = pos < line.size() ? hexValue(line.at(pos)) : -1;
            if (n < 0)
                return false;
            value = (value << 4) | static_cast<uint32_t>(n);
        }
        return true;
    };

    uint32_t value = 0;
    if (!hex(frame.extended ? 8 : 3, value) || value > (frame.extended ? 0x1FFFFFFFu : 0x7FFu))
        return k_error;
    frame.id = value;
    if (!hex(1, value) || (!frame.fd && value > 8))
        return k_error;
    frame.dlc = static_cast<uint8_t>(value);

    if (!frame.rtr)
    {
        // "for (i = 0; i < byte_count && parse_loc < len; i++)": short is accepted
        const int bytes = k_dlcToBytes.at(frame.dlc);
        for (int i = 0; i < bytes && pos < line.size(); ++i)
        {
            if (!hex(2, value))
                return k_error;
            frame.data.append(static_cast<char>(value));
        }
        frame.data = frame.data.leftJustified(bytes, '\0');
    }
    if (pos != line.size())
        return k_error;  // trailing characters

    // can_is_tx_allowed(): open, not bus monitoring
    if (!_onBus || _silentActive)
        return k_error;

    _txQueue.append(frame);
    return k_ok;  // CR now: queued, not yet sent
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
            _transmitted.append(frame);  // CANable / Elmue report nothing more
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
        // undefined content; WeAct (since V1.0.0.6) and Elmue leave it out.
        if (_fw == Firmware::CANable1 || _fw == Firmware::CANable2)
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
