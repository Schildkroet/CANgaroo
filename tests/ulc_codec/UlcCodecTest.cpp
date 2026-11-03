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

// uLC (uCAN USB LIN Converter) ASCII protocol encoding/decoding.
//
// Received lines come from a device over a serial link and may be truncated or
// garbage, so the malformed-input cases matter as much as the good ones. The
// encoder tests pin the exact command layout the firmware parses by position.

#include <QtTest>

#include "core/BusMessage.h"
#include "driver/UlcDriver/UlcFrameCodec.h"

class UlcCodecTest : public QObject
{
    Q_OBJECT

private slots:
    void decodesDataFrame();
    void decodesFullLengthFrame();
    void decodesLowercaseHex();
    void headerWithoutResponseIsFlagged();
    void decodedFrameClearsStaleErrorFlags();

    void rejectsEmptyLine();
    void rejectsReplyLines_data();
    void rejectsReplyLines();
    void rejectsIdAboveSixBits();
    void rejectsLengthAboveEight();
    void rejectsTruncatedPayload();
    void rejectsTrailingCharacters();
    void rejectsNonHexPayload();

    void recognisesFrameAcks();
    void parsesFirmwareVersion();
    void rejectsMalformedFirmwareVersion();
    void parsesFirmwareVersionCode();

    void encodesStandardBaudRates();
    void encodesCustomBaudRate();
    void rejectsUnencodableBaudRate();
    void encodesChecksumMode();

    void encodesFrame();
    void encodesFrameWithoutPayload();
    void encodesHeader();
    void encoderRejectsOutOfRange();
    void frameRoundTrip();

    void encodesPublisherSlot();
    void encodesSubscriberSlot();
    void slotFieldsSitWhereFirmwareParsesThem();
    void responseTimeoutMatchesFirmware();
    void frameTime();
};

void UlcCodecTest::decodesDataFrame()
{
    BusMessage msg;
    QVERIFY(ulc::parseFrameLine("t0163112233", msg));

    QVERIFY(msg.busType() == BusType::LIN);
    QCOMPARE(msg.getId(), 0x16u);
    QCOMPARE(msg.getLength(), uint8_t(3));
    QCOMPARE(msg.getByte(0), uint8_t(0x11));
    QCOMPARE(msg.getByte(1), uint8_t(0x22));
    QCOMPARE(msg.getByte(2), uint8_t(0x33));
    QVERIFY(!msg.isErrorFrame());
}

void UlcCodecTest::decodesFullLengthFrame()
{
    BusMessage msg;
    QVERIFY(ulc::parseFrameLine("t03F80011223344556677", msg));

    QCOMPARE(msg.getId(), 0x3Fu);
    QCOMPARE(msg.getLength(), uint8_t(8));
    QCOMPARE(msg.getByte(0), uint8_t(0x00));
    QCOMPARE(msg.getByte(7), uint8_t(0x77));
}

void UlcCodecTest::decodesLowercaseHex()
{
    BusMessage msg;
    QVERIFY(ulc::parseFrameLine("t02a2beef", msg));

    QCOMPARE(msg.getId(), 0x2Au);
    QCOMPARE(msg.getByte(0), uint8_t(0xBE));
    QCOMPARE(msg.getByte(1), uint8_t(0xEF));
}

// The firmware reports a header that no node answered as a zero-length frame.
void UlcCodecTest::headerWithoutResponseIsFlagged()
{
    BusMessage msg;
    QVERIFY(ulc::parseFrameLine("t0200", msg));

    QCOMPARE(msg.getId(), 0x20u);
    QCOMPARE(msg.getLength(), uint8_t(0));
    QVERIFY(msg.errorFlags().testFlag(BusError::LinNotResponded));
}

// A BusMessage reused by the caller must not keep the previous frame's flags.
void UlcCodecTest::decodedFrameClearsStaleErrorFlags()
{
    BusMessage msg;
    QVERIFY(ulc::parseFrameLine("t0200", msg));
    QVERIFY(msg.isErrorFrame());

    QVERIFY(ulc::parseFrameLine("t020111", msg));
    QVERIFY(!msg.isErrorFrame());
}

void UlcCodecTest::rejectsEmptyLine()
{
    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine("", msg));
}

// Replies to commands share the line format; none of them is a frame.
void UlcCodecTest::rejectsReplyLines_data()
{
    QTest::addColumn<QByteArray>("line");

    QTest::newRow("ack lower")   << QByteArray("z");
    QTest::newRow("ack upper")   << QByteArray("Z");
    QTest::newRow("hw version")  << QByteArray("V01");
    QTest::newRow("fw version")  << QByteArray("v0303");
    QTest::newRow("serial")      << QByteArray("N12345678");
    QTest::newRow("timed frame") << QByteArray("T0164301A5");
}

void UlcCodecTest::rejectsReplyLines()
{
    QFETCH(QByteArray, line);

    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine(line, msg));
}

void UlcCodecTest::rejectsIdAboveSixBits()
{
    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine("t0401AA", msg));
    QVERIFY(!ulc::parseFrameLine("t1001AA", msg));
}

void UlcCodecTest::rejectsLengthAboveEight()
{
    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine("t016900112233445566778899", msg));
}

void UlcCodecTest::rejectsTruncatedPayload()
{
    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine("t01631122", msg));
    QVERIFY(!ulc::parseFrameLine("t016311223", msg));
    QVERIFY(!ulc::parseFrameLine("t016", msg));
}

void UlcCodecTest::rejectsTrailingCharacters()
{
    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine("t016111FF", msg));
}

void UlcCodecTest::rejectsNonHexPayload()
{
    BusMessage msg;
    QVERIFY(!ulc::parseFrameLine("t0162GG11", msg));
    QVERIFY(!ulc::parseFrameLine("t0X62AA11", msg));
}

void UlcCodecTest::recognisesFrameAcks()
{
    QVERIFY(ulc::isFrameAck("z"));
    QVERIFY(ulc::isFrameAck("Z"));
    QVERIFY(!ulc::isFrameAck(""));
    QVERIFY(!ulc::isFrameAck("zz"));
    QVERIFY(!ulc::isFrameAck("\x07"));
}

void UlcCodecTest::parsesFirmwareVersion()
{
    QCOMPARE(ulc::parseFirmwareVersion("v0303"), QByteArray("3.3"));
    QCOMPARE(ulc::parseFirmwareVersion("v0A10"), QByteArray("10.16"));
}

void UlcCodecTest::rejectsMalformedFirmwareVersion()
{
    QVERIFY(ulc::parseFirmwareVersion("").isEmpty());
    QVERIFY(ulc::parseFirmwareVersion("v03").isEmpty());
    QVERIFY(ulc::parseFirmwareVersion("V0303").isEmpty());
    QVERIFY(ulc::parseFirmwareVersion("v03G3").isEmpty());
    QVERIFY(ulc::parseFirmwareVersion("v030300").isEmpty());
}

void UlcCodecTest::encodesStandardBaudRates()
{
    QCOMPARE(ulc::encodeBaudRate(9600), QByteArray("S2\r"));
    QCOMPARE(ulc::encodeBaudRate(19200), QByteArray("S1\r"));
}

void UlcCodecTest::encodesCustomBaudRate()
{
    QCOMPARE(ulc::encodeBaudRate(10417), QByteArray("t0428B1\r"));
    QCOMPARE(ulc::encodeBaudRate(2400), QByteArray("t040960\r"));
}

void UlcCodecTest::rejectsUnencodableBaudRate()
{
    QVERIFY(ulc::encodeBaudRate(0).isEmpty());
    QVERIFY(ulc::encodeBaudRate(0x10000).isEmpty());
}

void UlcCodecTest::encodesChecksumMode()
{
    QCOMPARE(ulc::encodeChecksum(true), QByteArray("r31\r"));
    QCOMPARE(ulc::encodeChecksum(false), QByteArray("r30\r"));
}

void UlcCodecTest::encodesFrame()
{
    const uint8_t data[] = {0x11, 0x22, 0x33};
    QCOMPARE(ulc::encodeFrame(0x16, data, 3), QByteArray("t0163112233\r"));
}

void UlcCodecTest::encodesFrameWithoutPayload()
{
    QCOMPARE(ulc::encodeFrame(0x3C, nullptr, 0), QByteArray("t03C0\r"));
}

void UlcCodecTest::encodesHeader()
{
    QCOMPARE(ulc::encodeHeader(0x21, 4), QByteArray("r0214\r"));
}

void UlcCodecTest::encoderRejectsOutOfRange()
{
    const uint8_t data[9] = {};
    QVERIFY(ulc::encodeFrame(0x40, data, 1).isEmpty());
    QVERIFY(ulc::encodeFrame(0x10, data, 9).isEmpty());
    QVERIFY(ulc::encodeHeader(0x40, 1).isEmpty());
    QVERIFY(ulc::encodeScheduleSlot(0x40, 1, true, data, 10, 10).isEmpty());
    QVERIFY(ulc::encodeScheduleSlot(0x10, 9, false, nullptr, 10, 10).isEmpty());
}

// What the driver sends in monitor mode is what the device reports back.
void UlcCodecTest::frameRoundTrip()
{
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    QByteArray line = ulc::encodeFrame(0x2B, data, 8);
    line.chop(1); // CR

    BusMessage msg;
    QVERIFY(ulc::parseFrameLine(line, msg));
    QCOMPARE(msg.getId(), 0x2Bu);
    QCOMPARE(msg.getLength(), uint8_t(8));
    for (uint8_t i = 0; i < 8; ++i)
        QCOMPARE(msg.getByte(i), data[i]);
}

void UlcCodecTest::encodesPublisherSlot()
{
    const uint8_t data[] = {0xAA, 0x55};
    QCOMPARE(ulc::encodeScheduleSlot(0x13, 2, true, data, 0x15, 0x0A),
             QByteArray("T013150A02AA55\r"));
}

void UlcCodecTest::encodesSubscriberSlot()
{
    const uint8_t ignored[] = {0xFF, 0xFF};
    QCOMPARE(ulc::encodeScheduleSlot(0x21, 2, false, ignored, 0x32, 0x05),
             QByteArray("R021320502\r"));
}

// Firmware addLinMasterRow(): ID at [2..3], delay at [4..5], timeout at
// [6..7], length at [9], data from [10].
void UlcCodecTest::slotFieldsSitWhereFirmwareParsesThem()
{
    const uint8_t data[] = {0x01, 0x02, 0x03};
    const QByteArray cmd = ulc::encodeScheduleSlot(0x2C, 3, true, data, 0x64, 0x07);

    QCOMPARE(ulc::parseHex(cmd, 2, 2), 0x2C);
    QCOMPARE(ulc::parseHex(cmd, 4, 2), 0x64);
    QCOMPARE(ulc::parseHex(cmd, 6, 2), 0x07);
    QCOMPARE(ulc::parseHex(cmd, 9, 1), 3);
    QCOMPARE(ulc::parseHex(cmd, 10, 2), 0x01);
    QCOMPARE(ulc::parseHex(cmd, 14, 2), 0x03);
    QCOMPARE(cmd.size(), 10 + 3 * 2 + 1);
}

// Same formula as the firmware's default for rows without a timeout.
void UlcCodecTest::responseTimeoutMatchesFirmware()
{
    QCOMPARE(ulc::responseTimeoutMs(8, 19200), uint8_t(10));   // (124 * 1400) / 19200 + 1
    QCOMPARE(ulc::responseTimeoutMs(0, 19200), uint8_t(4));
    QCOMPARE(ulc::responseTimeoutMs(8, 2400), uint8_t(73));
    QCOMPARE(ulc::responseTimeoutMs(8, 0), uint8_t(0xFF));
    QCOMPARE(ulc::responseTimeoutMs(8, 100), uint8_t(0xFF));   // clamped to one byte
}

void UlcCodecTest::parsesFirmwareVersionCode()
{
    QCOMPARE(ulc::parseFirmwareVersionCode("v0303"), 0x0303);
    QCOMPARE(ulc::parseFirmwareVersionCode("v0304"), 0x0304);
    QVERIFY(ulc::parseFirmwareVersionCode("v0303") < ulc::k_fwSlotDelayFromFrameStart);
    QVERIFY(ulc::parseFirmwareVersionCode("v0400") >= ulc::k_fwSlotDelayFromFrameStart);
    QCOMPARE(ulc::parseFirmwareVersionCode("V0303"), -1);
    QCOMPARE(ulc::parseFirmwareVersionCode("v03"), -1);
    QCOMPARE(ulc::parseFirmwareVersionCode(""), -1);
}

// Break + sync + PID (34 bits) and data + checksum (10 bits each), rounded.
void UlcCodecTest::frameTime()
{
    QCOMPARE(ulc::frameTimeMs(4, 19200), 4u);   // 84 bits = 4.38 ms
    QCOMPARE(ulc::frameTimeMs(8, 19200), 6u);   // 124 bits = 6.46 ms
    QCOMPARE(ulc::frameTimeMs(0, 19200), 2u);   // 44 bits = 2.29 ms
    QCOMPARE(ulc::frameTimeMs(8, 9600), 13u);   // 12.92 ms
    QCOMPARE(ulc::frameTimeMs(8, 0), 0u);
}

QTEST_APPLESS_MAIN(UlcCodecTest)

#include "UlcCodecTest.moc"
