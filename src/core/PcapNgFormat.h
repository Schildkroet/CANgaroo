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

// pcapng (IETF draft-ietf-opsawg-pcapng) block encoding for LINKTYPE_CAN_SOCKETCAN.
// Shared by the one-shot export in BusTrace and the streaming TraceRecorder.
//
// A file is one Section Header Block followed by Interface Description Blocks
// and Enhanced Packet Blocks. An IDB may appear anywhere in the section as long
// as it precedes the first EPB that references its index, which lets a recorder
// add interfaces as they show up. All fields are little-endian.

#include <QByteArray>
#include <QString>
#include <QtGlobal>

class BusMessage;

namespace PcapNgFormat
{

// Section Header Block with unspecified section length, so blocks can be
// appended without rewriting the header.
[[nodiscard]] QByteArray sectionHeaderBlock();

// Interface Description Block (LINKTYPE_CAN_SOCKETCAN, if_name option).
[[nodiscard]] QByteArray interfaceDescriptionBlock(const QString &name);

// `msg` as a SocketCAN can_frame (16 bytes) or canfd_frame (72 bytes), the
// packet data of LINKTYPE_CAN_SOCKETCAN in both pcap and pcapng.
[[nodiscard]] QByteArray socketCanFrame(const BusMessage &msg);

// Enhanced Packet Block carrying `msg` as a SocketCAN can_frame (16 bytes) or
// canfd_frame (72 bytes), microsecond timestamp. `interfaceIndex` is the 0-based
// position of the interface's IDB in the section.
[[nodiscard]] QByteArray enhancedPacketBlock(const BusMessage &msg, quint32 interfaceIndex);

}
