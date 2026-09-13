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

// Complete trace files in every export format, written from a plain list of
// messages. Free of Backend so whole files can be unit-tested and checked
// against reference readers; BusTrace::save() delegates here.

#include <functional>
#include <span>

#include <QString>

#include "core/BusMessage.h"
#include "core/TraceFileFormat.h"

class QIODevice;

namespace TraceFileWriter
{

// Interface label for candump lines and pcapng interface descriptions.
using InterfaceNameFn = std::function<QString(BusInterfaceId)>;

void write(QIODevice &out, TraceFileFormat format, std::span<const BusMessage> messages,
           const InterfaceNameFn &interfaceName);

// Linux candump -L lines.
void writeCanDump(QIODevice &out, std::span<const BusMessage> messages, const InterfaceNameFn &interfaceName);

// Vector ASC; times relative to the first message, channels numbered 1.. in order
// of first appearance. An empty trace still gets header and footer.
void writeVectorAsc(QIODevice &out, std::span<const BusMessage> messages);

// ASAM MDF 4.10 with one channel group of fixed-size records:
//   t (REAL, seconds since the first message, master)
//   | CAN_ID (canid_t, see core/SocketCan.h: flags, error classes for error frames)
//   | DLC (payload length) | Dir (0 = Rx, 1 = Tx) | DataBytes (64-byte array)
void writeVectorMdf(QIODevice &out, std::span<const BusMessage> messages);

// libpcap, microsecond timestamps, LINKTYPE_CAN_SOCKETCAN.
void writePcap(QIODevice &out, std::span<const BusMessage> messages);

// pcapng: one section, one interface description per interface in order of
// first appearance, then one enhanced packet per message.
void writePcapNg(QIODevice &out, std::span<const BusMessage> messages, const InterfaceNameFn &interfaceName);

}
