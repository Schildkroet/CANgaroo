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

// Text trace lines shared by the trace export and the TraceRecorder, and the ASC
// CAN FD parser used by the replay window.
//
// The CAN FD writer used to emit "<flags> 0 0 <len> <len>" instead of the Vector
// "<BRS> <ESI> <DLC> <len>". It survived because cangaroo's own replay parser
// read exactly that wrong layout back. So the reference lines here are verbatim
// python-can 4.6.1 output (can.ASCWriter / can.CanutilsLogWriter), and the DLC
// table is ISO 11898-1:2015 Table 5 -- never cangaroo output.

#include <QtTest>
#include <QRegularExpression>

#include "core/BusMessage.h"
#include "core/TraceLineFormat.h"

namespace
{

QStringList tokens(const QString &line)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return line.split(whitespace, Qt::SkipEmptyParts);
}

QByteArray sequence(int count)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.append(static_cast<char>(i));
    }
    return bytes;
}

BusMessage fdMessage(uint32_t id, bool extended, bool brs, bool rx, qint64 timestampUs, const QByteArray &data)
{
    BusMessage msg;
    msg.setFD(true);
    msg.setId(id);
    msg.setExtended(extended);
    msg.setBRS(brs);
    msg.setRX(rx);
    msg.setTimestamp_us(timestampUs);
    msg.setLength(static_cast<uint8_t>(data.size()));
    for (int i = 0; i < data.size(); ++i)
    {
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(data[i]));
    }
    return msg;
}

// python-can can.ASCWriter, verbatim, for:
//   Message(timestamp=0.00, arbitration_id=0x456,      is_fd, bitrate_switch, channel=1, rx, data=00..0B)
//   Message(timestamp=0.01, arbitration_id=0x18DAF110, is_fd, extended,       channel=0, tx, data=A5 x 64)
//   Message(timestamp=0.02, arbitration_id=0x7E0,      is_fd, bitrate_switch, channel=0, rx, data=01 02 03)
// (python-can channels are 0-based, ASC channels 1-based.)
const QString asc_fd_12 = QStringLiteral(
    " 0.000000 CANFD   2 Rx        456                                   1 0 9 12 "
    "00 01 02 03 04 05 06 07 08 09 0A 0B        0    0     3000        0        0        0        0        0");
const QString asc_fd_64 = QStringLiteral(" 0.010000 CANFD   1 Tx   18DAF110x                                   0 0 f 64 ")
    + QStringList(64, QStringLiteral("A5")).join(QLatin1Char(' '))
    + QStringLiteral("        0    0     1000        0        0        0        0        0");
const QString asc_fd_3 = QStringLiteral(
    " 0.020000 CANFD   1 Rx        7E0                                   1 0 3  3 "
    "01 02 03        0    0     3000        0        0        0        0        0");

}

class TraceLineFormatTest : public QObject
{
    Q_OBJECT

private slots:
    void dlcToLength_data();
    void dlcToLength();
    void lengthToDlc_data();
    void lengthToDlc();
    void ascFdLine_data();
    void ascFdLine();
    void canDumpFdLine_data();
    void canDumpFdLine();
    void canDumpErrorFrame_data();
    void canDumpErrorFrame();
    void parseAscCanFd_data();
    void parseAscCanFd();
    void parseAscCanFdRejectsMalformed_data();
    void parseAscCanFdRejectsMalformed();
};

void TraceLineFormatTest::dlcToLength_data()
{
    QTest::addColumn<int>("dlc");
    QTest::addColumn<int>("length");

    const int iso[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    for (int dlc = 0; dlc < 16; ++dlc)
    {
        QTest::addRow("dlc %d", dlc) << dlc << iso[dlc];
    }
    QTest::newRow("below range") << -1 << -1;
    QTest::newRow("above range") << 16 << -1;
}

void TraceLineFormatTest::dlcToLength()
{
    QFETCH(int, dlc);
    QFETCH(int, length);
    QCOMPARE(TraceLineFormat::canFdDlcToLength(dlc), length);
}

void TraceLineFormatTest::lengthToDlc_data()
{
    QTest::addColumn<int>("length");
    QTest::addColumn<int>("dlc");

    QTest::newRow("0") << 0 << 0;
    QTest::newRow("8") << 8 << 8;
    QTest::newRow("12") << 12 << 9;
    QTest::newRow("13 rounds up to 16") << 13 << 10;
    QTest::newRow("20") << 20 << 11;
    QTest::newRow("24") << 24 << 12;
    QTest::newRow("25 rounds up to 32") << 25 << 13;
    QTest::newRow("48") << 48 << 14;
    QTest::newRow("49 rounds up to 64") << 49 << 15;
    QTest::newRow("64") << 64 << 15;
}

void TraceLineFormatTest::lengthToDlc()
{
    QFETCH(int, length);
    QFETCH(int, dlc);
    QCOMPARE(TraceLineFormat::canFdLengthToDlc(length), dlc);
}

void TraceLineFormatTest::ascFdLine_data()
{
    QTest::addColumn<QString>("reference");
    QTest::addColumn<uint>("id");
    QTest::addColumn<bool>("extended");
    QTest::addColumn<bool>("brs");
    QTest::addColumn<bool>("rx");
    QTest::addColumn<int>("channel");
    QTest::addColumn<qint64>("timestampUs");
    QTest::addColumn<QByteArray>("data");

    QTest::newRow("12 bytes, BRS") << asc_fd_12 << 0x456u << false << true << true << 2 << qint64{0} << sequence(12);
    QTest::newRow("64 bytes, extended, TX") << asc_fd_64 << 0x18DAF110u << true << false << false << 1 << qint64{10000}
                                            << QByteArray(64, static_cast<char>(0xA5));
    QTest::newRow("3 bytes") << asc_fd_3 << 0x7E0u << false << true << true << 1 << qint64{20000} << QByteArray("\x01\x02\x03");
}

void TraceLineFormatTest::ascFdLine()
{
    QFETCH(QString, reference);
    QFETCH(uint, id);
    QFETCH(bool, extended);
    QFETCH(bool, brs);
    QFETCH(bool, rx);
    QFETCH(int, channel);
    QFETCH(qint64, timestampUs);
    QFETCH(QByteArray, data);

    const BusMessage msg = fdMessage(id, extended, brs, rx, timestampUs, data);
    const QString line = TraceLineFormat::ascLine(msg, 0.0, channel);

    // time CANFD channel dir id BRS ESI DLC length data...; python-can appends
    // optional duration/CRC/bit-timing fields after the data, which cangaroo omits.
    const qsizetype fieldCount = 9 + data.size();
    QCOMPARE(tokens(line).join(QLatin1Char(' ')).toUpper(),
             tokens(reference).mid(0, fieldCount).join(QLatin1Char(' ')).toUpper());
}

void TraceLineFormatTest::canDumpFdLine_data()
{
    QTest::addColumn<QString>("reference");
    QTest::addColumn<QString>("interfaceName");
    QTest::addColumn<uint>("id");
    QTest::addColumn<bool>("extended");
    QTest::addColumn<bool>("brs");
    QTest::addColumn<QByteArray>("data");

    // python-can can.CanutilsLogWriter; its trailing R/T direction marker is not
    // part of the candump -L format and is ignored.
    QTest::newRow("12 bytes, BRS") << QStringLiteral("(0.000000) can1 456##1000102030405060708090A0B R")
                                   << QStringLiteral("can1") << 0x456u << false << true << sequence(12);
    QTest::newRow("64 bytes, extended")
        << QStringLiteral("(0.000000) can0 18DAF110##0") + QString(QStringLiteral("A5")).repeated(64) + QStringLiteral(" T")
        << QStringLiteral("can0") << 0x18DAF110u << true << false << QByteArray(64, static_cast<char>(0xA5));
    QTest::newRow("3 bytes") << QStringLiteral("(0.000000) can0 7E0##1010203 R")
                             << QStringLiteral("can0") << 0x7E0u << false << true << QByteArray("\x01\x02\x03");
}

void TraceLineFormatTest::canDumpFdLine()
{
    QFETCH(QString, reference);
    QFETCH(QString, interfaceName);
    QFETCH(uint, id);
    QFETCH(bool, extended);
    QFETCH(bool, brs);
    QFETCH(QByteArray, data);

    const BusMessage msg = fdMessage(id, extended, brs, true, 0, data);
    QCOMPARE(tokens(TraceLineFormat::canDumpLine(msg, interfaceName)), tokens(reference).mid(0, 3));
}

void TraceLineFormatTest::canDumpErrorFrame_data()
{
    QTest::addColumn<uint>("flags");
    QTest::addColumn<QString>("expected");

    // can_id = CAN_ERR_FLAG | error classes, 8-byte payload, per <linux/can/error.h>.
    // python-can's CanutilsLogWriter writes an unclassified error as 20000080#.
    QTest::newRow("generic")
        << uint(BusError::Generic) << QStringLiteral("(0.000000) can0 20000080#0000000000000000");
    QTest::newRow("bus off: CAN_ERR_BUSOFF")
        << uint(BusError::BusOff) << QStringLiteral("(0.000000) can0 20000040#0000000000000000");
    QTest::newRow("restarted: CAN_ERR_RESTARTED")
        << uint(BusError::Restarted) << QStringLiteral("(0.000000) can0 20000100#0000000000000000");
    QTest::newRow("warning: CAN_ERR_CRTL, data[1] CAN_ERR_CRTL_RX_WARNING | TX_WARNING")
        << uint(BusError::ErrorWarning) << QStringLiteral("(0.000000) can0 20000004#000C000000000000");
    QTest::newRow("passive: CAN_ERR_CRTL, data[1] CAN_ERR_CRTL_RX_PASSIVE | TX_PASSIVE")
        << uint(BusError::ErrorPassive) << QStringLiteral("(0.000000) can0 20000004#0030000000000000");
    QTest::newRow("active: CAN_ERR_CRTL, data[1] CAN_ERR_CRTL_ACTIVE")
        << uint(BusError::ErrorActive) << QStringLiteral("(0.000000) can0 20000004#0040000000000000");
    QTest::newRow("stuff: CAN_ERR_PROT | BUSERROR, data[2] CAN_ERR_PROT_STUFF")
        << uint(BusError::Stuff) << QStringLiteral("(0.000000) can0 20000088#0000040000000000");
    QTest::newRow("crc: CAN_ERR_PROT | BUSERROR, data[3] CAN_ERR_PROT_LOC_CRC_SEQ")
        << uint(BusError::Crc) << QStringLiteral("(0.000000) can0 20000088#0000000800000000");
    QTest::newRow("overrun: CAN_ERR_CRTL, data[1] CAN_ERR_CRTL_RX_OVERFLOW")
        << uint(BusError::Overrun) << QStringLiteral("(0.000000) can0 20000004#0001000000000000");
    QTest::newRow("ack + bit: CAN_ERR_ACK | PROT | BUSERROR, data[2] CAN_ERR_PROT_BIT")
        << (uint(BusError::Ack) | uint(BusError::Bit)) << QStringLiteral("(0.000000) can0 200000A8#0000010000000000");
    QTest::newRow("tx timeout: CAN_ERR_TX_TIMEOUT")
        << uint(BusError::TxTimeout) << QStringLiteral("(0.000000) can0 20000001#0000000000000000");
}

void TraceLineFormatTest::canDumpErrorFrame()
{
    QFETCH(uint, flags);
    QFETCH(QString, expected);

    BusMessage msg;
    msg.setErrorFlags(BusErrors(QFlag(static_cast<int>(flags))));
    QVERIFY(msg.isErrorFrame());
    QCOMPARE(TraceLineFormat::canDumpLine(msg, QStringLiteral("can0")), expected);
}

void TraceLineFormatTest::parseAscCanFd_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<uint>("id");
    QTest::addColumn<bool>("extended");
    QTest::addColumn<bool>("brs");
    QTest::addColumn<bool>("rx");
    QTest::addColumn<int>("channel");
    QTest::addColumn<QByteArray>("data");

    QTest::newRow("python-can 12 bytes") << asc_fd_12 << 0x456u << false << true << true << 2 << sequence(12);
    QTest::newRow("python-can 64 bytes") << asc_fd_64 << 0x18DAF110u << true << false << false << 1
                                         << QByteArray(64, static_cast<char>(0xA5));
    QTest::newRow("python-can 3 bytes") << asc_fd_3 << 0x7E0u << false << true << true << 1 << QByteArray("\x01\x02\x03");

    // Vector allows a symbolic frame name between id and BRS.
    QTest::newRow("symbolic name") << QStringLiteral(" 0.020000 CANFD   1 Rx        7E0  EngineData  1 0 3  3 01 02 03")
                                   << 0x7E0u << false << true << true << 1 << QByteArray("\x01\x02\x03");

    // Written by cangaroo before the writer was fixed; existing traces must keep loading.
    QTest::newRow("legacy cangaroo layout")
        << QStringLiteral("   0.030000 CANFD   2 Rx             456 1 0 0 12 12 00 01 02 03 04 05 06 07 08 09 0A 0B ")
        << 0x456u << false << true << true << 2 << sequence(12);
}

void TraceLineFormatTest::parseAscCanFd()
{
    QFETCH(QString, line);
    QFETCH(uint, id);
    QFETCH(bool, extended);
    QFETCH(bool, brs);
    QFETCH(bool, rx);
    QFETCH(int, channel);
    QFETCH(QByteArray, data);

    BusMessage msg;
    QVERIFY(TraceLineFormat::parseAscCanFdLine(tokens(line), msg));
    QVERIFY(msg.isFD());
    QCOMPARE(msg.getId(), static_cast<uint32_t>(id));
    QCOMPARE(msg.isExtended(), extended);
    QCOMPARE(msg.isBRS(), brs);
    QCOMPARE(msg.isRX(), rx);
    QCOMPARE(static_cast<int>(msg.getInterfaceId()), channel);
    QCOMPARE(static_cast<int>(msg.getLength()), data.size());
    for (int i = 0; i < data.size(); ++i)
    {
        QCOMPARE(msg.getByte(static_cast<uint8_t>(i)), static_cast<uint8_t>(data[i]));
    }
}

void TraceLineFormatTest::parseAscCanFdRejectsMalformed_data()
{
    QTest::addColumn<QString>("line");

    const QString twelveBytes = QStringLiteral("00 01 02 03 04 05 06 07 08 09 0A 0B");
    QTest::newRow("DLC disagrees with length") << QStringLiteral(" 0.0 CANFD 1 Rx 456 1 0 8 12 ") + twelveBytes;
    QTest::newRow("data truncated") << QStringLiteral(" 0.0 CANFD 1 Rx 456 1 0 9 12 00 01 02");
    QTest::newRow("bad id") << QStringLiteral(" 0.0 CANFD 1 Rx zz 1 0 3 3 01 02 03");
    QTest::newRow("byte out of range") << QStringLiteral(" 0.0 CANFD 1 Rx 456 1 0 3 3 01 1FF 03");
    QTest::newRow("too short") << QStringLiteral(" 0.0 CANFD 1 Rx");
    QTest::newRow("classic CAN line") << QStringLiteral("   0.000000 1  123             Rx   d 8 11 22 33 44 55 66 77 88");
}

void TraceLineFormatTest::parseAscCanFdRejectsMalformed()
{
    QFETCH(QString, line);

    BusMessage msg;
    QVERIFY(!TraceLineFormat::parseAscCanFdLine(tokens(line), msg));
}

QTEST_APPLESS_MAIN(TraceLineFormatTest)

#include "TraceLineFormatTest.moc"
