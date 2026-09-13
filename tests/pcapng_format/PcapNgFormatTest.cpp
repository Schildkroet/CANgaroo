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

// pcapng block encoding shared by the trace export and the TraceRecorder.
//
// Expected bytes are assembled by hand from the pcapng specification
// (draft-ietf-opsawg-pcapng: SHB 0x0A0D0D0A, IDB 1, EPB 6, byte-order magic
// 0x1A2B3C4D, options if_name=2 / shb_userappl=4), LINKTYPE_CAN_SOCKETCAN = 227
// (tcpdump.org link-layer types, can_id in network byte order) and
// <linux/can.h> (CAN_EFF_FLAG 0x80000000, CAN_RTR_FLAG 0x40000000,
// CAN_ERR_FLAG 0x20000000, CANFD_BRS 0x01, CAN_MTU 16, CANFD_MTU 72).
// Never from cangaroo output.

#include <QtTest>

#include "core/BusMessage.h"
#include "core/PcapNgFormat.h"

namespace
{

QByteArray hex(const char *spaced)
{
    return QByteArray::fromHex(QByteArray(spaced).replace(' ', ""));
}

BusMessage frame(uint32_t id, bool extended, qint64 timestampUs, const QByteArray &data)
{
    BusMessage msg;
    msg.setId(id);
    msg.setExtended(extended);
    msg.setTimestamp_us(timestampUs);
    msg.setLength(static_cast<uint8_t>(data.size()));
    for (int i = 0; i < data.size(); ++i)
    {
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(data[i]));
    }
    return msg;
}

quint32 le32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

}

class PcapNgFormatTest : public QObject
{
    Q_OBJECT

private slots:
    void sectionHeaderBlock();
    void interfaceDescriptionBlock();
    void classicFrame();
    void canIdFlags_data();
    void canIdFlags();
    void fdFrame();
};

void PcapNgFormatTest::sectionHeaderBlock()
{
    const QByteArray expected = hex(
        "0A0D0D0A"            // block type SHB
        "2C000000"            // total length 44
        "4D3C2B1A"            // byte-order magic, little-endian
        "0100 0000"           // version 1.0
        "FFFFFFFFFFFFFFFF"    // section length unspecified
        "0400 0800"           // shb_userappl, 8 bytes
        "43414E6761726F6F"    // "CANgaroo"
        "0000 0000"           // opt_endofopt
        "2C000000");          // total length again
    QCOMPARE(PcapNgFormat::sectionHeaderBlock(), expected);
}

void PcapNgFormatTest::interfaceDescriptionBlock()
{
    const QByteArray expected = hex(
        "01000000"            // block type IDB
        "24000000"            // total length 36
        "E300 0000"           // LINKTYPE_CAN_SOCKETCAN (227), reserved
        "48000000"            // snaplen 72 (CANFD_MTU)
        "0200 0500"           // if_name, 5 bytes
        "7663616E30 000000"   // "vcan0" + padding to 4
        "0000 0000"           // opt_endofopt
        "24000000");
    QCOMPARE(PcapNgFormat::interfaceDescriptionBlock(QStringLiteral("vcan0")), expected);
}

void PcapNgFormatTest::classicFrame()
{
    // 0x100000002 us: exercises both timestamp halves.
    const BusMessage msg = frame(0x123, false, 0x100000002LL, QByteArray("\xAA\xBB\xCC", 3));

    const QByteArray expected = hex(
        "06000000"            // block type EPB
        "30000000"            // total length 48
        "01000000"            // interface index 1
        "01000000 02000000"   // timestamp high, low
        "10000000 10000000"   // captured / original length: CAN_MTU
        "00000123"            // can_id, network byte order
        "03 000000"           // can_dlc + padding
        "AABBCC0000000000"    // data[8]
        "30000000");
    QCOMPARE(PcapNgFormat::enhancedPacketBlock(msg, 1), expected);
}

void PcapNgFormatTest::canIdFlags_data()
{
    QTest::addColumn<uint>("id");
    QTest::addColumn<bool>("extended");
    QTest::addColumn<bool>("rtr");
    QTest::addColumn<bool>("error");
    QTest::addColumn<QByteArray>("canId");

    QTest::newRow("standard")       << 0x123u      << false << false << false << hex("00000123");
    QTest::newRow("extended")       << 0x18DAF110u << true  << false << false << hex("98DAF110");
    QTest::newRow("extended + RTR") << 0x18DAF110u << true  << true  << false << hex("D8DAF110");
    // Unclassified error: CAN_ERR_FLAG | CAN_ERR_BUSERROR (<linux/can/error.h>);
    // the message's own identifier plays no part in an error frame.
    QTest::newRow("error frame")    << 0x040u      << false << false << true  << hex("20000080");
}

void PcapNgFormatTest::canIdFlags()
{
    QFETCH(uint, id);
    QFETCH(bool, extended);
    QFETCH(bool, rtr);
    QFETCH(bool, error);
    QFETCH(QByteArray, canId);

    BusMessage msg = frame(id, extended, 0, QByteArray());
    msg.setRTR(rtr);
    msg.setErrorFrame(error);

    const QByteArray block = PcapNgFormat::enhancedPacketBlock(msg, 0);
    QCOMPARE(block.mid(28, 4), canId);
}

void PcapNgFormatTest::fdFrame()
{
    QByteArray data;
    for (int i = 0; i < 48; ++i)
    {
        data.append(static_cast<char>(i + 1));
    }
    BusMessage msg = frame(0x456, false, 0, data);
    msg.setFD(true);
    msg.setBRS(true);

    const QByteArray block = PcapNgFormat::enhancedPacketBlock(msg, 0);

    QCOMPARE(block.size(), 104);                 // 28 header + CANFD_MTU 72 + 4 trailer
    QCOMPARE(le32(block, 4), quint32(104));
    QCOMPARE(le32(block, 100), quint32(104));
    QCOMPARE(le32(block, 20), quint32(72));      // captured length
    QCOMPARE(le32(block, 24), quint32(72));      // original length
    QCOMPARE(block.mid(28, 4), hex("00000456"));
    QCOMPARE(static_cast<quint8>(block[32]), quint8(48));   // len
    QCOMPARE(static_cast<quint8>(block[33]), quint8(0x01)); // flags: CANFD_BRS
    QCOMPARE(block.mid(34, 2), hex("0000"));
    QCOMPARE(block.mid(36, 48), data);
    QCOMPARE(block.mid(84, 16), QByteArray(16, '\0'));      // unused payload zeroed
}

QTEST_APPLESS_MAIN(PcapNgFormatTest)

#include "PcapNgFormatTest.moc"
