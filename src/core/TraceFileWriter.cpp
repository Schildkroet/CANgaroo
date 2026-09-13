/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "TraceFileWriter.h"

#include <array>

#include <QDataStream>
#include <QDateTime>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QTextStream>

#include "core/PcapNgFormat.h"
#include "core/SocketCan.h"
#include "core/TraceLineFormat.h"

// VERSION_STRING is injected by qmake for the application; tests build without it.
#ifndef VERSION_STRING
#define VERSION_STRING "dev"
#endif

namespace
{

// TXBLOCK / MDBLOCK size: 24-byte header + text + NUL, padded to 8 bytes.
[[nodiscard]] quint64 mdfTextBlockSize(const QByteArray &text) noexcept
{
    return (24 + static_cast<quint64>(text.size()) + 1 + 7) & ~quint64(7);
}

struct MdfChannel
{
    QByteArray name;
    QByteArray unit;
    quint8 type;        // cn_type: 0 = fixed length, 2 = master
    quint8 syncType;    // cn_sync_type: 0 = none, 1 = time
    quint8 dataType;    // cn_data_type: 0 = UINT LE, 4 = REAL LE, 10 = byte array
    quint32 byteOffset;
    quint32 bitCount;
    quint64 offset = 0;
    quint64 nameOffset = 0;
    quint64 unitOffset = 0;
};

constexpr quint32 mdf_record_size = 8 + 4 + 1 + 1 + 64;  // t, CAN_ID, DLC, Dir, DataBytes

}

namespace TraceFileWriter
{

void write(QIODevice &out, TraceFileFormat format, std::span<const BusMessage> messages,
           const InterfaceNameFn &interfaceName)
{
    switch (format)
    {
        case TraceFileFormat::CanDump:   writeCanDump(out, messages, interfaceName); return;
        case TraceFileFormat::VectorAsc: writeVectorAsc(out, messages); return;
        case TraceFileFormat::VectorMdf: writeVectorMdf(out, messages); return;
        case TraceFileFormat::Pcap:      writePcap(out, messages); return;
        case TraceFileFormat::PcapNg:    writePcapNg(out, messages, interfaceName); return;
    }
}

void writeCanDump(QIODevice &out, std::span<const BusMessage> messages, const InterfaceNameFn &interfaceName)
{
    QTextStream stream(&out);
    for (const BusMessage &msg : messages)
    {
        stream << TraceLineFormat::canDumpLine(msg, interfaceName(msg.getInterfaceId())) << '\n';
    }
}

void writeVectorAsc(QIODevice &out, std::span<const BusMessage> messages)
{
    QTextStream stream(&out);

    const QDateTime start = messages.empty() ? QDateTime::currentDateTime() : messages.front().getDateTime();
    const double tStart = messages.empty() ? 0.0 : messages.front().getFloatTimestamp();
    stream << TraceLineFormat::ascHeader(start);

    QMap<BusInterfaceId, int> channels;
    for (const BusMessage &msg : messages)
    {
        const BusInterfaceId id = msg.getInterfaceId();
        auto it = channels.constFind(id);
        if (it == channels.cend())
        {
            it = channels.insert(id, static_cast<int>(channels.size()) + 1);
        }
        stream << TraceLineFormat::ascLine(msg, tStart, it.value()) << '\n';
    }

    stream << TraceLineFormat::ascFooter() << '\n';
}

void writeVectorMdf(QIODevice &out, std::span<const BusMessage> messages)
{
    // Block sizes follow the ASAM MDF 4.1 block definitions: 24-byte common
    // header, 8 bytes per link, then the block's data section.
    constexpr quint64 szId = 64;
    constexpr quint64 szHd = 104;  // 6 links + 32 data
    constexpr quint64 szFh = 56;   // 2 links + 16 data
    constexpr quint64 szDg = 64;   // 4 links + 8 data
    constexpr quint64 szCg = 104;  // 6 links + 32 data
    constexpr quint64 szCn = 160;  // 8 links + 72 data

    QDataStream ds(&out);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::DoublePrecision);

    const auto recordCount = static_cast<quint64>(messages.size());
    const quint64 startTimeNs = messages.empty()
        ? static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1'000'000ULL
        : static_cast<quint64>(messages.front().getTimestamp_us()) * 1000ULL;
    const double tStart = messages.empty() ? 0.0 : messages.front().getFloatTimestamp();

    // The file history comment must be an MDBLOCK with <FHcomment>.
    const QByteArray fhComment =
        QByteArray("<FHcomment xmlns=\"http://www.asam.net/mdf/v4\"><TX>CANgaroo trace export</TX>"
                   "<tool_id>CANgaroo</tool_id><tool_vendor>CANgaroo</tool_vendor><tool_version>")
        + QByteArray(VERSION_STRING) + QByteArray("</tool_version></FHcomment>");
    const QByteArray acquisitionName("CAN");

    std::array<MdfChannel, 5> channels = {{
        { "t",         "s", 2, 1, 4,  0,  64  },
        { "CAN_ID",    "",  0, 0, 0,  8,  32  },
        { "DLC",       "",  0, 0, 0,  12, 8   },
        { "Dir",       "",  0, 0, 0,  13, 8   },
        { "DataBytes", "",  0, 0, 10, 14, 512 },
    }};

    // --- Block offsets ---
    quint64 off = szId + szHd;
    const quint64 oFh = off;      off += szFh;
    const quint64 oMdFh = off;    off += mdfTextBlockSize(fhComment);
    const quint64 oDg = off;      off += szDg;
    const quint64 oCg = off;      off += szCg;
    const quint64 oTxCg = off;    off += mdfTextBlockSize(acquisitionName);
    for (MdfChannel &ch : channels)
    {
        ch.offset = off;          off += szCn;
        ch.nameOffset = off;      off += mdfTextBlockSize(ch.name);
        if (!ch.unit.isEmpty())
        {
            ch.unitOffset = off;  off += mdfTextBlockSize(ch.unit);
        }
    }
    const quint64 oDt = off;

    auto zeros = [&ds](quint64 count)
    {
        for (quint64 i = 0; i < count; ++i)
        {
            ds << quint8(0);
        }
    };
    auto blockHeader = [&ds](const char *id, quint64 length, quint64 linkCount)
    {
        ds.writeRawData(id, 4);
        ds << quint32(0);  // reserved
        ds << length;
        ds << linkCount;
    };
    auto textBlock = [&](const char *id, const QByteArray &text)
    {
        const quint64 size = mdfTextBlockSize(text);
        blockHeader(id, size, 0);
        ds.writeRawData(text.constData(), static_cast<int>(text.size()));
        zeros(size - 24 - static_cast<quint64>(text.size()));  // NUL terminator + padding
    };

    // ===== IDBLOCK (no common header) =====
    ds.writeRawData("MDF     ", 8);
    ds.writeRawData("4.10    ", 8);
    ds.writeRawData("CANgaroo", 8);
    zeros(4);                 // id_reserved1
    ds << quint16(410);       // id_ver
    zeros(30);                // id_reserved2
    ds << quint16(0);         // id_unfin_flags: finalized
    ds << quint16(0);         // id_custom_unfin_flags

    // ===== HDBLOCK =====
    blockHeader("##HD", szHd, 6);
    ds << oDg;                // hd_dg_first
    ds << oFh;                // hd_fh_first
    ds << quint64(0);         // hd_ch_first
    ds << quint64(0);         // hd_at_first
    ds << quint64(0);         // hd_ev_first
    ds << quint64(0);         // hd_md_comment
    ds << startTimeNs;        // hd_start_time_ns (UTC)
    ds << qint16(0);          // hd_tz_offset_min
    ds << qint16(0);          // hd_dst_offset_min
    ds << quint8(0);          // hd_time_flags: UTC, offsets not valid
    ds << quint8(0);          // hd_time_class: local PC reference time
    ds << quint8(0);          // hd_flags: start angle / distance not valid
    ds << quint8(0);          // hd_reserved
    ds << 0.0;                // hd_start_angle_rad
    ds << 0.0;                // hd_start_distance_m

    // ===== FHBLOCK =====
    blockHeader("##FH", szFh, 2);
    ds << quint64(0);         // fh_fh_next
    ds << oMdFh;              // fh_md_comment
    ds << startTimeNs;        // fh_time_ns
    ds << qint16(0);          // fh_tz_offset_min
    ds << qint16(0);          // fh_dst_offset_min
    ds << quint8(0);          // fh_time_flags
    zeros(3);                 // fh_reserved
    textBlock("##MD", fhComment);

    // ===== DGBLOCK =====
    blockHeader("##DG", szDg, 4);
    ds << quint64(0);         // dg_dg_next
    ds << oCg;                // dg_cg_first
    ds << oDt;                // dg_data
    ds << quint64(0);         // dg_md_comment
    ds << quint8(0);          // dg_rec_id_size: single channel group
    zeros(7);                 // dg_reserved

    // ===== CGBLOCK =====
    blockHeader("##CG", szCg, 6);
    ds << quint64(0);         // cg_cg_next
    ds << channels[0].offset; // cg_cn_first
    ds << oTxCg;              // cg_tx_acq_name
    ds << quint64(0);         // cg_si_acq_source
    ds << quint64(0);         // cg_sr_first
    ds << quint64(0);         // cg_md_comment
    ds << quint64(0);         // cg_record_id
    ds << recordCount;        // cg_cycle_count
    ds << quint16(0);         // cg_flags
    ds << quint16(0);         // cg_path_separator
    zeros(4);                 // cg_reserved
    ds << mdf_record_size;    // cg_data_bytes
    ds << quint32(0);         // cg_inval_bytes
    textBlock("##TX", acquisitionName);

    // ===== CNBLOCKs =====
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        const MdfChannel &ch = channels[i];
        blockHeader("##CN", szCn, 8);
        ds << (i + 1 < channels.size() ? channels[i + 1].offset : quint64(0));  // cn_cn_next
        ds << quint64(0);     // cn_composition
        ds << ch.nameOffset;  // cn_tx_name
        ds << quint64(0);     // cn_si_source
        ds << quint64(0);     // cn_cc_conversion
        ds << quint64(0);     // cn_data
        ds << ch.unitOffset;  // cn_md_unit
        ds << quint64(0);     // cn_md_comment
        ds << ch.type;
        ds << ch.syncType;
        ds << ch.dataType;
        ds << quint8(0);      // cn_bit_offset
        ds << ch.byteOffset;
        ds << ch.bitCount;
        ds << quint32(0);     // cn_flags: ranges and limits not valid
        ds << quint32(0);     // cn_inval_bit_pos
        ds << quint8(0);      // cn_precision
        ds << quint8(0);      // cn_reserved
        ds << quint16(0);     // cn_attachment_count
        for (int k = 0; k < 6; ++k)
        {
            ds << 0.0;        // value range, limit, extended limit
        }
        textBlock("##TX", ch.name);
        if (!ch.unit.isEmpty())
        {
            textBlock("##TX", ch.unit);
        }
    }

    // ===== DTBLOCK =====
    blockHeader("##DT", 24 + recordCount * mdf_record_size, 0);
    for (const BusMessage &msg : messages)
    {
        const quint8 len = qMin<quint8>(msg.getLength(), 64);
        // canid_t as in <linux/can.h>: standard and extended frames with the same
        // number stay distinct, error frames keep their error class.
        const quint32 canId = SocketCan::canId(msg);

        ds << (msg.getFloatTimestamp() - tStart);
        ds << canId;
        ds << len;
        ds << quint8(msg.isRX() ? 0 : 1);
        for (int j = 0; j < 64; ++j)
        {
            ds << (j < len ? msg.getByte(static_cast<uint8_t>(j)) : quint8(0));
        }
    }
}

void writePcap(QIODevice &out, std::span<const BusMessage> messages)
{
    QDataStream ds(&out);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Global header
    ds << quint32(0xA1B2C3D4);  // magic: microsecond timestamps
    ds << quint16(2) << quint16(4);
    ds << qint32(0);            // thiszone: UTC
    ds << quint32(0);           // sigfigs
    ds << quint32(72);          // snaplen: CANFD_MTU
    ds << quint32(227);         // LINKTYPE_CAN_SOCKETCAN

    for (const BusMessage &msg : messages)
    {
        const QByteArray frame = PcapNgFormat::socketCanFrame(msg);
        const int64_t tsUs = msg.getTimestamp_us();
        ds << static_cast<quint32>(tsUs / 1'000'000);
        ds << static_cast<quint32>(tsUs % 1'000'000);
        ds << static_cast<quint32>(frame.size());  // incl_len
        ds << static_cast<quint32>(frame.size());  // orig_len
        ds.writeRawData(frame.constData(), static_cast<int>(frame.size()));
    }
}

void writePcapNg(QIODevice &out, std::span<const BusMessage> messages, const InterfaceNameFn &interfaceName)
{
    QMap<BusInterfaceId, quint32> interfaceIndex;
    QList<BusInterfaceId> interfaceOrder;
    for (const BusMessage &msg : messages)
    {
        const BusInterfaceId id = msg.getInterfaceId();
        if (!interfaceIndex.contains(id))
        {
            interfaceIndex.insert(id, static_cast<quint32>(interfaceOrder.size()));
            interfaceOrder.append(id);
        }
    }

    out.write(PcapNgFormat::sectionHeaderBlock());
    for (const BusInterfaceId id : std::as_const(interfaceOrder))
    {
        out.write(PcapNgFormat::interfaceDescriptionBlock(interfaceName(id)));
    }
    for (const BusMessage &msg : messages)
    {
        out.write(PcapNgFormat::enhancedPacketBlock(msg, interfaceIndex.value(msg.getInterfaceId())));
    }
}

}
