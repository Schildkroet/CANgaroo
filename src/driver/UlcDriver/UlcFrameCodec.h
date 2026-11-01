/*

  Copyright (c) 2026 Lukasz Juranek

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

// Pure encoding/decoding of the uLC (uCAN USB LIN Converter) ASCII protocol,
// split out of UlcInterface so it can be exercised without a serial port.
//
// The protocol is SLCAN-style: every command and every reply ends with CR.
//   RX frame      t0IIL<data>     IIL = 6-bit LIN ID (2 hex) and length (1 hex)
//   frame / slot  t0IIL<data>     publisher row (master/slave) or one-shot TX (monitor)
//   header / slot r0IIL           subscriber row, or header-only request (monitor)
//   timed slot    T0IIPPTT0L<data> / R0IIPPTT0L   PP = delay before the slot, TT = response timeout (ms)
//   schedule      r1ff0 start, r2ff0 clear
//   checksum      r31 classic, r30 enhanced          (replies "Z")
//   baud rate     S2 = 9600, S1 = 19200, t04BBBB = any (BBBB hex, replies "Z")
//
// Frame-type commands are acknowledged with "z" (lower case command) or "Z"
// (upper case command). A slot the device cannot take is answered with a bare
// CR, a command it does not understand with BEL (0x07).
// Firmware reference: https://github.com/uCAN-LIN/LinUSBConverter

#include <cstdint>

#include <QByteArray>

#include "core/BusMessage.h"

namespace ulc
{

constexpr uint8_t k_maxLinId = 0x3F;
constexpr uint8_t k_maxDataLen = 8;
// Schedule rows the firmware keeps, shared by all frames of the running table.
constexpr int k_maxScheduleSlots = 16;

inline const QByteArray k_cmdClose         = QByteArrayLiteral("C\r");
inline const QByteArray k_cmdOpenMaster    = QByteArrayLiteral("O\r");
inline const QByteArray k_cmdOpenSlave     = QByteArrayLiteral("L\r");
inline const QByteArray k_cmdOpenMonitor   = QByteArrayLiteral("l\r");
inline const QByteArray k_cmdHwVersion     = QByteArrayLiteral("V\r");
inline const QByteArray k_cmdFwVersion     = QByteArrayLiteral("v\r");
inline const QByteArray k_cmdStartSchedule = QByteArrayLiteral("r1ff0\r");
inline const QByteArray k_cmdClearSchedule = QByteArrayLiteral("r2ff0\r");

[[nodiscard]] constexpr char hexNibble(uint8_t v) noexcept
{
    v &= 0x0F;
    return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('A' + v - 10);
}

[[nodiscard]] constexpr int fromHexNibble(char c) noexcept
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

// Parses `count` hex digits at `pos`; -1 if the line is too short or not hex.
[[nodiscard]] inline int parseHex(const QByteArray &line, int pos, int count) noexcept
{
    if (pos < 0 || line.size() < pos + count) { return -1; }

    int value = 0;
    for (int i = 0; i < count; ++i)
    {
        const int nibble = fromHexNibble(line.at(pos + i));
        if (nibble < 0) { return -1; }
        value = (value << 4) | nibble;
    }
    return value;
}

inline void appendHexByte(QByteArray &out, uint8_t b)
{
    out.append(hexNibble(b >> 4));
    out.append(hexNibble(b));
}

// True for a reply that acknowledges a frame-type command ("z" / "Z").
[[nodiscard]] inline bool isFrameAck(const QByteArray &line) noexcept
{
    return line == "z" || line == "Z";
}

// Decodes one received LIN frame line (without the trailing CR) into msg.
//
// Sets bus type, ID, length and payload; timestamp, interface id and direction
// are the caller's business. A header that no node answered arrives with
// length 0 and is flagged LinNotResponded. The device drops frames with a bad
// checksum itself, so they never show up here.
// Returns false and leaves msg unspecified for anything that is not a frame.
[[nodiscard]] inline bool parseFrameLine(const QByteArray &line, BusMessage &msg) noexcept
{
    constexpr int headerLen = 5; // 't' + '0' + 2 ID digits + length digit

    if (line.size() < headerLen || line.at(0) != 't') { return false; }

    // The firmware pads the ID to SLCAN's three digits; the first one is always 0.
    const int id = parseHex(line, 1, 3);
    if (id < 0 || id > k_maxLinId) { return false; }

    const int len = parseHex(line, 4, 1);
    if (len < 0 || len > k_maxDataLen) { return false; }

    if (line.size() != headerLen + len * 2) { return false; }

    msg.setBusType(BusType::LIN);
    msg.setId(static_cast<uint32_t>(id));
    msg.setLength(static_cast<uint8_t>(len));
    msg.setErrorFrame(false); // clears all error flags

    for (int i = 0; i < len; ++i)
    {
        const int b = parseHex(line, headerLen + i * 2, 2);
        if (b < 0) { return false; }
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(b));
    }

    if (len == 0)
    {
        msg.setErrorFlag(BusError::LinNotResponded);
    }

    return true;
}

// "v0303" -> "3.3"; empty if the reply is not a uLC firmware version.
[[nodiscard]] inline QByteArray parseFirmwareVersion(const QByteArray &line)
{
    if (line.size() != 5 || line.at(0) != 'v') { return {}; }

    const int major = parseHex(line, 1, 2);
    const int minor = parseHex(line, 3, 2);
    if (major < 0 || minor < 0) { return {}; }

    return QByteArray::number(major) + '.' + QByteArray::number(minor);
}

// Baud rate command. 9600 and 19200 use the classic S commands every firmware
// knows; anything else needs the t04 command of firmware 3.x. Empty if the
// rate does not fit the command.
[[nodiscard]] inline QByteArray encodeBaudRate(unsigned baud)
{
    if (baud == 9600)  { return QByteArrayLiteral("S2\r"); }
    if (baud == 19200) { return QByteArrayLiteral("S1\r"); }
    if (baud == 0 || baud > 0xFFFF) { return {}; }

    QByteArray cmd = "t04";
    appendHexByte(cmd, static_cast<uint8_t>(baud >> 8));
    appendHexByte(cmd, static_cast<uint8_t>(baud));
    cmd.append('\r');
    return cmd;
}

[[nodiscard]] inline QByteArray encodeChecksum(bool classic)
{
    return classic ? QByteArrayLiteral("r31\r") : QByteArrayLiteral("r30\r");
}

// Frame with payload: a publisher row in master/slave mode (also how a running
// row's payload is updated), a one-shot header + data in monitor mode.
// Empty if id or length are out of range.
[[nodiscard]] inline QByteArray encodeFrame(uint8_t id, const uint8_t *data, uint8_t len)
{
    if (id > k_maxLinId || len > k_maxDataLen) { return {}; }

    QByteArray cmd = "t0";
    appendHexByte(cmd, id);
    cmd.append(hexNibble(len));
    for (uint8_t i = 0; i < len; ++i)
        appendHexByte(cmd, data[i]);
    cmd.append('\r');
    return cmd;
}

// Header only: a subscriber row in master/slave mode, a header request that
// another node answers in monitor mode.
[[nodiscard]] inline QByteArray encodeHeader(uint8_t id, uint8_t len)
{
    if (id > k_maxLinId || len > k_maxDataLen) { return {}; }

    QByteArray cmd = "r0";
    appendHexByte(cmd, id);
    cmd.append(hexNibble(len));
    cmd.append('\r');
    return cmd;
}

// Response timeout the firmware itself uses for rows without one: 1.4 times
// the nominal frame time (header + data + checksum, 10 bits per byte), in ms.
[[nodiscard]] constexpr uint8_t responseTimeoutMs(uint8_t len, unsigned baud) noexcept
{
    if (baud == 0) { return 0xFF; }
    const unsigned ms = ((len * 10u + 44u) * 1400u) / baud + 1u;
    return ms > 0xFF ? uint8_t(0xFF) : static_cast<uint8_t>(ms);
}

// Master schedule slot with explicit timing. delayMs is the gap before the
// slot's header; data is only used (and may only be non-null) for publisher
// slots. Empty if id or length are out of range.
[[nodiscard]] inline QByteArray encodeScheduleSlot(uint8_t id, uint8_t len, bool publisher,
                                                   const uint8_t *data, uint8_t delayMs,
                                                   uint8_t timeoutMs)
{
    if (id > k_maxLinId || len > k_maxDataLen) { return {}; }

    QByteArray cmd = publisher ? "T0" : "R0";
    appendHexByte(cmd, id);
    appendHexByte(cmd, delayMs);
    appendHexByte(cmd, timeoutMs);
    cmd.append('0');
    cmd.append(hexNibble(len));
    if (publisher)
    {
        for (uint8_t i = 0; i < len; ++i)
            appendHexByte(cmd, data ? data[i] : uint8_t(0));
    }
    cmd.append('\r');
    return cmd;
}

} // namespace ulc
