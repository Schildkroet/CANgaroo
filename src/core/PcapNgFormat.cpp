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

#include "PcapNgFormat.h"

#include <QDataStream>
#include <QIODevice>
#include <QtEndian>

#include "core/BusMessage.h"
#include "core/SocketCan.h"

namespace
{

constexpr quint32 BT_SHB = 0x0A0D0D0A;
constexpr quint32 BT_IDB = 0x00000001;
constexpr quint32 BT_EPB = 0x00000006;

constexpr quint32 BYTE_ORDER_MAGIC = 0x1A2B3C4D;
constexpr quint16 PCAPNG_VERSION_MAJ = 1;
constexpr quint16 PCAPNG_VERSION_MIN = 0;

constexpr quint16 LINKTYPE_CAN_SOCKETCAN = 227;
constexpr quint32 SNAPLEN = 72;

constexpr quint8  CANFD_BRS    = 0x01;

constexpr quint16 OPT_ENDOFOPT   = 0;
constexpr quint16 OPT_IF_NAME    = 2;
constexpr quint16 OPT_SHB_USERAPPL = 4;

constexpr quint32 CAN_MTU   = 16;
constexpr quint32 CANFD_MTU = 72;

[[nodiscard]] quint32 padded4(quint32 len) noexcept
{
    return (len + 3) & ~quint32(3);
}

// Padding so that `unpadded` bytes end on a 4-byte boundary.
void pad4(QDataStream &ds, quint32 unpadded)
{
    for (quint32 p = unpadded; p < padded4(unpadded); ++p)
    {
        ds << quint8(0);
    }
}

// Option: code (2) + length (2) + value + padding.
void writeOption(QDataStream &ds, quint16 code, const QByteArray &value)
{
    ds << code;
    ds << quint16(value.size());
    ds.writeRawData(value.constData(), static_cast<int>(value.size()));
    pad4(ds, static_cast<quint32>(value.size()));
}

void writeEndOfOpt(QDataStream &ds)
{
    ds << OPT_ENDOFOPT << quint16(0);
}

}

namespace PcapNgFormat
{

QByteArray socketCanFrame(const BusMessage &msg)
{
    // struct can_frame   { canid_t can_id; __u8 len; __u8 __pad; __u8 __res0; __u8 len8_dlc; __u8 data[8]; };
    // struct canfd_frame { canid_t can_id; __u8 len; __u8 flags; __u8 __res0; __u8 __res1; __u8 data[64]; };
    // can_id is in network byte order for LINKTYPE_CAN_SOCKETCAN. len8_dlc is
    // only meaningful for classic DLC codes above 8, which BusMessage cannot hold.
    const bool fd = msg.isFD() && !msg.isErrorFrame();
    QByteArray frame(fd ? CANFD_MTU : CAN_MTU, '\0');
    qToBigEndian<quint32>(SocketCan::canId(msg), frame.data());

    if (msg.isErrorFrame())
    {
        // Error frames are classic frames carrying the 8-byte error payload.
        const SocketCan::ErrorFrame error = SocketCan::errorFrame(msg);
        frame[4] = static_cast<char>(SocketCan::err_dlc);
        for (int j = 0; j < SocketCan::err_dlc; ++j)
        {
            frame[8 + j] = static_cast<char>(error.data[static_cast<std::size_t>(j)]);
        }
        return frame;
    }

    const quint8 len = qMin<quint8>(msg.getLength(), fd ? 64 : 8);
    frame[4] = static_cast<char>(len);
    if (fd && msg.isBRS())
    {
        frame[5] = static_cast<char>(CANFD_BRS);
    }
    for (int j = 0; j < len; ++j)
    {
        frame[8 + j] = static_cast<char>(msg.getByte(static_cast<uint8_t>(j)));
    }
    return frame;
}

QByteArray sectionHeaderBlock()
{
    const QByteArray appName("CANgaroo");
    const quint32 optLen = 4 + padded4(static_cast<quint32>(appName.size())) + 4;  // userappl + endofopt
    // type(4) + total_length(4) + bom(4) + major(2) + minor(2) + section_length(8) + options + total_length(4)
    const quint32 totalLen = 4 + 4 + 4 + 2 + 2 + 8 + optLen + 4;

    QByteArray bytes;
    QDataStream ds(&bytes, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << BT_SHB;
    ds << totalLen;
    ds << BYTE_ORDER_MAGIC;
    ds << PCAPNG_VERSION_MAJ;
    ds << PCAPNG_VERSION_MIN;
    ds << quint64(0xFFFFFFFFFFFFFFFF);  // section length: unspecified
    writeOption(ds, OPT_SHB_USERAPPL, appName);
    writeEndOfOpt(ds);
    ds << totalLen;
    return bytes;
}

QByteArray interfaceDescriptionBlock(const QString &name)
{
    const QByteArray nameUtf8 = name.toUtf8();
    const quint32 optLen = 4 + padded4(static_cast<quint32>(nameUtf8.size())) + 4;  // if_name + endofopt
    // type(4) + total_length(4) + linktype(2) + reserved(2) + snaplen(4) + options + total_length(4)
    const quint32 totalLen = 4 + 4 + 2 + 2 + 4 + optLen + 4;

    QByteArray bytes;
    QDataStream ds(&bytes, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << BT_IDB;
    ds << totalLen;
    ds << LINKTYPE_CAN_SOCKETCAN;
    ds << quint16(0);  // reserved
    ds << SNAPLEN;
    writeOption(ds, OPT_IF_NAME, nameUtf8);
    writeEndOfOpt(ds);
    ds << totalLen;
    return bytes;
}

QByteArray enhancedPacketBlock(const BusMessage &msg, quint32 interfaceIndex)
{
    // Timestamp in the interface's ts_resol, default microseconds.
    const int64_t tsUs = msg.getTimestamp_us();
    const auto tsHigh = static_cast<quint32>(static_cast<quint64>(tsUs) >> 32);
    const auto tsLow = static_cast<quint32>(static_cast<quint64>(tsUs) & 0xFFFFFFFF);

    const QByteArray frame = socketCanFrame(msg);
    const auto capturedLen = static_cast<quint32>(frame.size());
    // type(4) + total_length(4) + interface_id(4) + ts_high(4) + ts_low(4)
    // + captured_len(4) + original_len(4) + packet data (padded) + total_length(4)
    const quint32 totalLen = 4 + 4 + 4 + 4 + 4 + 4 + 4 + padded4(capturedLen) + 4;

    QByteArray bytes;
    QDataStream ds(&bytes, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << BT_EPB;
    ds << totalLen;
    ds << interfaceIndex;
    ds << tsHigh;
    ds << tsLow;
    ds << capturedLen;
    ds << capturedLen;  // original length

    ds.writeRawData(frame.constData(), static_cast<int>(frame.size()));
    pad4(ds, capturedLen);
    ds << totalLen;
    return bytes;
}

}
