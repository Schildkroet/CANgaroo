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

// Whole trace files as written by the export (BusTrace::save -> TraceFileWriter).
//
// Expected structure comes from the format definitions and the input frames:
//   candump -L       "(<sec>.<usec>) <iface> <id>#<data>"
//   Vector ASC       header block, relative times, 1-based channels, footer
//   libpcap          0xA1B2C3D4 global header v2.4, 16-byte record headers
//   pcapng           SHB, IDBs before use, EPB interface indices
//   ASAM MDF 4.1     block ids and sizes (24-byte header + 8 per link + data),
//                    link targets, one time master, non-overlapping channels
//   <linux/can.h>    can_frame / canfd_frame layout, flag bits
// Line and block encoders are covered by trace_line_format and pcapng_format;
// tests/format_compat additionally reads these files with python-can, scapy
// and asammdf.

#include <cstring>
#include <span>

#include <QtTest>
#include <QBuffer>
#include <QRegularExpression>

#include "core/BusMessage.h"
#include "core/TraceFileWriter.h"

namespace
{

// No digit separators in this file: qmake's moc scanner reads ' as a character
// literal, misses Q_OBJECT and generates no TraceFileWriterTest.moc.
constexpr qint64 base_us = 1757000000000000LL;

QByteArray hex(const char *spaced)
{
    return QByteArray::fromHex(QByteArray(spaced).replace(' ', ""));
}

quint16 le16(const QByteArray &bytes, quint64 offset)
{
    return qFromLittleEndian<quint16>(bytes.constData() + offset);
}

quint32 le32(const QByteArray &bytes, quint64 offset)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

quint64 le64(const QByteArray &bytes, quint64 offset)
{
    return qFromLittleEndian<quint64>(bytes.constData() + offset);
}

double leDouble(const QByteArray &bytes, quint64 offset)
{
    const quint64 bits = le64(bytes, offset);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

QString interfaceName(BusInterfaceId id)
{
    if (id == 7) { return QStringLiteral("vcan0"); }
    if (id == 3) { return QStringLiteral("vcan1"); }
    return QStringLiteral("if%1").arg(id);
}

BusMessage frame(qint64 timestampUs, int iface, uint32_t id, bool extended, bool rx, const QByteArray &data)
{
    BusMessage msg;
    msg.setTimestamp_us(timestampUs);
    msg.setInterfaceId(static_cast<BusInterfaceId>(iface));
    msg.setId(id);
    msg.setExtended(extended);
    msg.setRX(rx);
    msg.setLength(static_cast<uint8_t>(data.size()));
    for (int i = 0; i < data.size(); ++i)
    {
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(data[i]));
    }
    return msg;
}

QByteArray written(TraceFileFormat format, const QVector<BusMessage> &messages)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    TraceFileWriter::write(buffer, format,
                           std::span<const BusMessage>(messages.constData(), static_cast<std::size_t>(messages.size())),
                           interfaceName);
    return buffer.data();
}

QStringList lines(const QByteArray &content)
{
    return QString::fromUtf8(content).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QStringList tokens(const QString &line)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return line.split(whitespace, Qt::SkipEmptyParts);
}

// --- MDF4 ---

struct MdfBlock
{
    QByteArray id;
    quint64 offset = 0;
    quint64 length = 0;
    quint64 linkCount = 0;
};

MdfBlock mdfBlock(const QByteArray &file, quint64 offset)
{
    MdfBlock block;
    block.offset = offset;
    if (offset != 0 && offset + 24 <= static_cast<quint64>(file.size()))
    {
        block.id = file.mid(static_cast<qsizetype>(offset), 4);
        block.length = le64(file, offset + 8);
        block.linkCount = le64(file, offset + 16);
    }
    return block;
}

quint64 mdfLink(const QByteArray &file, const MdfBlock &block, quint64 index)
{
    return le64(file, block.offset + 24 + 8 * index);
}

quint64 mdfData(const MdfBlock &block)
{
    return block.offset + 24 + 8 * block.linkCount;
}

QString mdfText(const QByteArray &file, quint64 offset)
{
    const MdfBlock block = mdfBlock(file, offset);
    const QByteArray body = file.mid(static_cast<qsizetype>(offset + 24), static_cast<qsizetype>(block.length) - 24);
    return QString::fromUtf8(body.left(body.indexOf('\0')));
}

// Every link of `block` is null or points at an 8-byte aligned block inside the file.
void verifyMdfLinks(const QByteArray &file, const MdfBlock &block)
{
    for (quint64 i = 0; i < block.linkCount; ++i)
    {
        const quint64 target = mdfLink(file, block, i);
        if (target == 0)
        {
            continue;
        }
        const QByteArray what = block.id + " link " + QByteArray::number(i);
        QVERIFY2(target % 8 == 0, what.constData());
        QVERIFY2(target + 24 <= static_cast<quint64>(file.size()), what.constData());
        QVERIFY2(file.mid(static_cast<qsizetype>(target), 2) == "##", what.constData());
    }
    QVERIFY2(block.length % 8 == 0, block.id.constData());
}

struct MdfChannel
{
    QString name;
    quint8 type = 0;
    quint8 syncType = 0;
    quint8 dataType = 0;
    quint32 byteOffset = 0;
    quint32 bitCount = 0;
};

void verifyMdf4(const QByteArray &file, const QVector<BusMessage> &messages)
{
    // IDBLOCK
    QVERIFY(file.size() >= 64);
    QCOMPARE(file.mid(0, 8), QByteArray("MDF     "));
    QCOMPARE(file.mid(8, 8), QByteArray("4.10    "));
    QCOMPARE(le16(file, 28), quint16(410));   // id_ver
    QCOMPARE(le16(file, 60), quint16(0));     // id_unfin_flags: finalized

    // HDBLOCK: 6 links + 32 bytes of data
    const MdfBlock hd = mdfBlock(file, 64);
    QCOMPARE(hd.id, QByteArray("##HD"));
    QCOMPARE(hd.linkCount, quint64(6));
    QCOMPARE(hd.length, quint64(24 + 6 * 8 + 32));
    verifyMdfLinks(file, hd);
    if (QTest::currentTestFailed()) { return; }
    if (!messages.isEmpty())
    {
        QCOMPARE(le64(file, mdfData(hd)), static_cast<quint64>(messages.first().getTimestamp_us()) * 1000);
    }
    QCOMPARE(static_cast<quint8>(file[static_cast<qsizetype>(mdfData(hd) + 12)]), quint8(0));  // time flags: UTC

    // FHBLOCK with an XML <FHcomment> in an MDBLOCK
    const MdfBlock fh = mdfBlock(file, mdfLink(file, hd, 1));
    QCOMPARE(fh.id, QByteArray("##FH"));
    QCOMPARE(fh.linkCount, quint64(2));
    QCOMPARE(fh.length, quint64(24 + 2 * 8 + 16));
    verifyMdfLinks(file, fh);
    const MdfBlock fhComment = mdfBlock(file, mdfLink(file, fh, 1));
    QCOMPARE(fhComment.id, QByteArray("##MD"));
    const QString xml = mdfText(file, fhComment.offset);
    QVERIFY(xml.startsWith(QLatin1String("<FHcomment")));
    QVERIFY(xml.contains(QLatin1String("<tool_id>")));
    QVERIFY(xml.contains(QLatin1String("<tool_vendor>")));
    QVERIFY(xml.contains(QLatin1String("<tool_version>")));

    // DGBLOCK: 4 links + 8 bytes
    const MdfBlock dg = mdfBlock(file, mdfLink(file, hd, 0));
    QCOMPARE(dg.id, QByteArray("##DG"));
    QCOMPARE(dg.linkCount, quint64(4));
    QCOMPARE(dg.length, quint64(24 + 4 * 8 + 8));
    verifyMdfLinks(file, dg);
    QCOMPARE(static_cast<quint8>(file[static_cast<qsizetype>(mdfData(dg))]), quint8(0));  // rec_id_size

    // CGBLOCK: 6 links + 32 bytes
    const MdfBlock cg = mdfBlock(file, mdfLink(file, dg, 1));
    QCOMPARE(cg.id, QByteArray("##CG"));
    QCOMPARE(cg.linkCount, quint64(6));
    QCOMPARE(cg.length, quint64(24 + 6 * 8 + 32));
    verifyMdfLinks(file, cg);
    if (QTest::currentTestFailed()) { return; }
    const quint64 cgData = mdfData(cg);
    QCOMPARE(le64(file, cgData + 8), static_cast<quint64>(messages.size()));  // cg_cycle_count
    const quint32 recordSize = le32(file, cgData + 24);                         // cg_data_bytes
    QCOMPARE(le32(file, cgData + 28), quint32(0));                              // cg_inval_bytes
    QCOMPARE(mdfText(file, mdfLink(file, cg, 2)), QStringLiteral("CAN"));

    // CNBLOCKs: 8 links + 72 bytes each
    QMap<QString, MdfChannel> channels;
    int masters = 0;
    for (quint64 cnOffset = mdfLink(file, cg, 1); cnOffset != 0;)
    {
        const MdfBlock cn = mdfBlock(file, cnOffset);
        QCOMPARE(cn.id, QByteArray("##CN"));
        QCOMPARE(cn.linkCount, quint64(8));
        QCOMPARE(cn.length, quint64(24 + 8 * 8 + 72));
        verifyMdfLinks(file, cn);
        if (QTest::currentTestFailed()) { return; }

        const quint64 d = mdfData(cn);
        MdfChannel ch;
        ch.name = mdfText(file, mdfLink(file, cn, 2));
        ch.type = static_cast<quint8>(file[static_cast<qsizetype>(d)]);
        ch.syncType = static_cast<quint8>(file[static_cast<qsizetype>(d + 1)]);
        ch.dataType = static_cast<quint8>(file[static_cast<qsizetype>(d + 2)]);
        QCOMPARE(static_cast<quint8>(file[static_cast<qsizetype>(d + 3)]), quint8(0));  // bit offset
        ch.byteOffset = le32(file, d + 4);
        ch.bitCount = le32(file, d + 8);
        QVERIFY2(quint64(ch.byteOffset) * 8 + ch.bitCount <= quint64(recordSize) * 8, qPrintable(ch.name));

        if (ch.type == 2)  // master channel
        {
            ++masters;
            QCOMPARE(ch.syncType, quint8(1));    // time
            QCOMPARE(ch.dataType, quint8(4));    // REAL little-endian
            QCOMPARE(ch.bitCount, quint32(64));
            QCOMPARE(mdfText(file, mdfLink(file, cn, 6)), QStringLiteral("s"));
        }
        channels.insert(ch.name, ch);
        cnOffset = mdfLink(file, cn, 0);
    }
    QCOMPARE(masters, 1);
    QCOMPARE(channels.keys(), QStringList({ "CAN_ID", "DLC", "DataBytes", "Dir", "t" }));
    QCOMPARE(channels["DataBytes"].dataType, quint8(10));   // byte array
    QCOMPARE(channels["DataBytes"].bitCount, quint32(512)); // room for a full CAN FD payload

    QList<MdfChannel> byOffset = channels.values();
    std::sort(byOffset.begin(), byOffset.end(),
              [](const MdfChannel &a, const MdfChannel &b) { return a.byteOffset < b.byteOffset; });
    for (qsizetype i = 1; i < byOffset.size(); ++i)
    {
        const MdfChannel &prev = byOffset[i - 1];
        QVERIFY2(quint64(prev.byteOffset) * 8 + prev.bitCount <= quint64(byOffset[i].byteOffset) * 8,
                 qPrintable(prev.name + " overlaps " + byOffset[i].name));
    }

    // DTBLOCK and records
    const MdfBlock dt = mdfBlock(file, mdfLink(file, dg, 2));
    QCOMPARE(dt.id, QByteArray("##DT"));
    QCOMPARE(dt.length, 24 + static_cast<quint64>(messages.size()) * recordSize);
    QVERIFY(dt.offset + dt.length <= static_cast<quint64>(file.size()));

    for (qsizetype i = 0; i < messages.size(); ++i)
    {
        const BusMessage &msg = messages[i];
        const quint64 record = dt.offset + 24 + static_cast<quint64>(i) * recordSize;
        const double expectedTime = msg.getFloatTimestamp() - messages.first().getFloatTimestamp();

        quint32 canId = msg.getId();
        if (msg.isExtended())   { canId |= 0x80000000u; }  // CAN_EFF_FLAG
        if (msg.isRTR())        { canId |= 0x40000000u; }  // CAN_RTR_FLAG
        if (msg.isErrorFrame()) { canId |= 0x20000000u; }  // CAN_ERR_FLAG

        QVERIFY(qAbs(leDouble(file, record + channels["t"].byteOffset) - expectedTime) < 1e-9);
        QCOMPARE(le32(file, record + channels["CAN_ID"].byteOffset), canId);
        QCOMPARE(static_cast<quint8>(file[static_cast<qsizetype>(record + channels["DLC"].byteOffset)]), msg.getLength());
        QCOMPARE(static_cast<quint8>(file[static_cast<qsizetype>(record + channels["Dir"].byteOffset)]),
                 quint8(msg.isRX() ? 0 : 1));
        QByteArray payload;
        for (int j = 0; j < msg.getLength(); ++j)
        {
            payload.append(static_cast<char>(msg.getByte(static_cast<uint8_t>(j))));
        }
        QCOMPARE(file.mid(static_cast<qsizetype>(record + channels["DataBytes"].byteOffset), payload.size()), payload);
    }
}

}

class TraceFileWriterTest : public QObject
{
    Q_OBJECT

private slots:
    void canDumpFile();
    void ascFile();
    void ascEmptyTraceIsValid();
    void pcapFile();
    void pcapEmptyTrace();
    void pcapngFile();
    void pcapngEmptyTrace();
    void mdf4File();
    void mdf4EmptyTraceIsValid();
};

void TraceFileWriterTest::canDumpFile()
{
    const QVector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 1500, 3, 0x456, false, false, hex("AA")),
    };
    QCOMPARE(lines(written(TraceFileFormat::CanDump, messages)),
             QStringList({ "(1757000000.000000) vcan0 123#112233", "(1757000000.001500) vcan1 456#AA" }));
}

void TraceFileWriterTest::ascFile()
{
    const QVector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 10000, 3, 0x456, false, false, hex("AA")),
        frame(base_us + 25000, 7, 0x124, false, true, hex("BB")),
    };
    const QStringList content = lines(written(TraceFileFormat::VectorAsc, messages));

    QCOMPARE(content.size(), 6 + 3 + 1);
    QVERIFY(content[0].startsWith(QLatin1String("date ")));
    QCOMPARE(content[1], QStringLiteral("base hex  timestamps absolute"));
    QVERIFY(content[4].startsWith(QLatin1String("Begin Triggerblock ")));
    QCOMPARE(content[5].trimmed(), QStringLiteral("0.000000 Start of measurement"));
    QCOMPARE(content.last(), QStringLiteral("End TriggerBlock"));

    // Times relative to the first frame; channels numbered by first appearance.
    QCOMPARE(tokens(content[6]).mid(0, 4), QStringList({ "0.000000", "1", "123", "Rx" }));
    QCOMPARE(tokens(content[7]).mid(0, 4), QStringList({ "0.010000", "2", "456", "Tx" }));
    QCOMPARE(tokens(content[8]).mid(0, 4), QStringList({ "0.025000", "1", "124", "Rx" }));
}

void TraceFileWriterTest::ascEmptyTraceIsValid()
{
    const QStringList content = lines(written(TraceFileFormat::VectorAsc, {}));
    QCOMPARE(content.size(), 7);
    QVERIFY(content[0].startsWith(QLatin1String("date ")));
    QCOMPARE(content.last(), QStringLiteral("End TriggerBlock"));
}

void TraceFileWriterTest::pcapFile()
{
    const QByteArray fdData = hex("000102030405060708090A0B");
    BusMessage fd = frame(2500000, 7, 0x18DAF110, true, true, fdData);
    fd.setFD(true);
    fd.setBRS(true);
    const QVector<BusMessage> messages = { frame(1000002, 7, 0x123, false, true, hex("AABBCC")), fd };

    QByteArray expected = hex(
        "D4C3B2A1 0200 0400"                  // magic (usec timestamps), version 2.4
        "00000000 00000000"                   // thiszone, sigfigs
        "48000000 E3000000"                   // snaplen 72, LINKTYPE_CAN_SOCKETCAN
        "01000000 02000000 10000000 10000000" // 1 s, 2 us, incl_len = orig_len = CAN_MTU
        "00000123 03 00 00 00"                // can_id, len, __pad, __res0, len8_dlc
        "AABBCC0000000000"
        "02000000 20A10700 48000000 48000000" // 2 s, 500000 us, CANFD_MTU
        "98DAF110 0C 01 00 00");              // CAN_EFF_FLAG | id, len 12, CANFD_BRS
    expected += fdData + QByteArray(64 - fdData.size(), '\0');

    QCOMPARE(written(TraceFileFormat::Pcap, messages), expected);
}

void TraceFileWriterTest::pcapEmptyTrace()
{
    QCOMPARE(written(TraceFileFormat::Pcap, {}), hex("D4C3B2A1 0200 0400 00000000 00000000 48000000 E3000000"));
}

void TraceFileWriterTest::pcapngFile()
{
    const QVector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("11")),
        frame(base_us + 1, 3, 0x124, false, true, hex("22")),
        frame(base_us + 2, 7, 0x125, false, true, hex("33")),
    };
    const QByteArray file = written(TraceFileFormat::PcapNg, messages);

    struct Block { quint32 type; QByteArray bytes; };
    QList<Block> blocks;
    for (qsizetype pos = 0; pos < file.size();)
    {
        QVERIFY(pos + 12 <= file.size());
        const quint32 length = le32(file, static_cast<quint64>(pos + 4));
        QVERIFY(length >= 12 && length % 4 == 0 && pos + length <= file.size());
        QCOMPARE(le32(file, static_cast<quint64>(pos + length - 4)), length);
        blocks.append({ le32(file, static_cast<quint64>(pos)), file.mid(pos, length) });
        pos += length;
    }

    QCOMPARE(blocks.size(), 1 + 2 + 3);
    QCOMPARE(blocks[0].type, quint32(0x0A0D0D0A));

    // Interfaces described up front in order of first appearance, name in if_name.
    auto ifName = [](const QByteArray &idb)
    {
        return le16(idb, 16) == 2 ? QString::fromUtf8(idb.mid(20, le16(idb, 18))) : QString();
    };
    QCOMPARE(blocks[1].type, quint32(1));
    QCOMPARE(le16(blocks[1].bytes, 8), quint16(227));
    QCOMPARE(ifName(blocks[1].bytes), QStringLiteral("vcan0"));
    QCOMPARE(blocks[2].type, quint32(1));
    QCOMPARE(ifName(blocks[2].bytes), QStringLiteral("vcan1"));

    const quint32 expectedInterface[] = { 0, 1, 0 };
    for (int i = 0; i < 3; ++i)
    {
        const QByteArray &epb = blocks[3 + i].bytes;
        QCOMPARE(blocks[3 + i].type, quint32(6));
        QCOMPARE(le32(epb, 8), expectedInterface[i]);
        const quint64 timestamp = (quint64(le32(epb, 12)) << 32) | le32(epb, 16);
        QCOMPARE(timestamp, static_cast<quint64>(messages[i].getTimestamp_us()));
    }
}

void TraceFileWriterTest::pcapngEmptyTrace()
{
    const QByteArray file = written(TraceFileFormat::PcapNg, {});
    QVERIFY(file.size() >= 28);
    QCOMPARE(le32(file, 0), quint32(0x0A0D0D0A));
    QCOMPARE(le32(file, 4), static_cast<quint32>(file.size()));  // nothing but the section header
}

void TraceFileWriterTest::mdf4File()
{
    BusMessage fd = frame(base_us + 20500, 7, 0x456, false, true, QByteArray(64, static_cast<char>(0x5A)));
    fd.setFD(true);
    fd.setBRS(true);
    const QVector<BusMessage> messages = {
        frame(base_us, 7, 0x123, false, true, hex("112233")),
        frame(base_us + 10000, 3, 0x18DAF110, true, false, hex("0102030405060708")),
        fd,
    };
    verifyMdf4(written(TraceFileFormat::VectorMdf, messages), messages);
}

void TraceFileWriterTest::mdf4EmptyTraceIsValid()
{
    verifyMdf4(written(TraceFileFormat::VectorMdf, {}), {});
}

QTEST_APPLESS_MAIN(TraceFileWriterTest)

#include "TraceFileWriterTest.moc"
