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

// Line-oriented text trace formats. Shared by the one-shot export in BusTrace
// and the streaming TraceRecorder so both produce identical output.

#include <array>

#include <QDateTime>
#include <QString>
#include <QStringList>

class BusMessage;

namespace TraceLineFormat
{

// CAN FD DLC code (0x0-0xF) -> payload length in bytes, ISO 11898-1:2015 Table 5.
inline constexpr std::array<int, 16> can_fd_dlc_lengths = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
};

// Returns -1 for a code outside 0x0-0xF.
[[nodiscard]] constexpr int canFdDlcToLength(int dlc) noexcept
{
    return (dlc >= 0 && dlc < 16) ? can_fd_dlc_lengths[static_cast<std::size_t>(dlc)] : -1;
}

// Smallest DLC code whose length holds `length` bytes.
[[nodiscard]] constexpr int canFdLengthToDlc(int length) noexcept
{
    for (int dlc = 0; dlc < 16; ++dlc)
    {
        if (can_fd_dlc_lengths[static_cast<std::size_t>(dlc)] >= length)
        {
            return dlc;
        }
    }
    return 15;
}

// Vector ASC header block; every line, including the last, ends in '\n'.
[[nodiscard]] QString ascHeader(const QDateTime &start);

// One ASC event line without trailing newline. `tStart` is the absolute
// timestamp (seconds) that relative times are measured from.
[[nodiscard]] QString ascLine(const BusMessage &msg, double tStart, int channel);

// Closing ASC line without trailing newline.
[[nodiscard]] QString ascFooter();

// One Linux candump line without trailing newline.
[[nodiscard]] QString canDumpLine(const BusMessage &msg, const QString &interfaceName);

// Parses a whitespace-split ASC CAN FD event ("<time> CANFD <channel> ...") into
// `msg`: id, flags, direction, channel as interface id, length and data. The
// timestamp is left to the caller. Accepts the Vector layout, with or without a
// symbolic frame name, and the layout cangaroo wrote before it was fixed:
//   Vector:   <id> [name] <BRS> <ESI> <DLC hex> <data length> <data...>
//   legacy:   <id> <flags> 0 0 <length> <length> <data...>
// Returns false for anything malformed.
[[nodiscard]] bool parseAscCanFdLine(const QStringList &parts, BusMessage &msg);

}
