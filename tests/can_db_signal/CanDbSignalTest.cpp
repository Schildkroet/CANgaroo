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

// CanDbSignal: raw <-> physical conversion, sign extension, and the multiplexer
// gate. These decide what number the user actually sees in the trace, so an
// error here is silently wrong output rather than a crash -- the same failure
// mode as the big-endian bug in issue #34.

#include <limits>

#include <QtTest>

#include "core/BusMessage.h"
#include "db/model/CanDbMessage.h"
#include "db/model/CanDbSignal.h"

namespace
{

// Mirrors DbcParser's Motorola start-bit conversion (DbcParser.cpp).
[[nodiscard]] uint16_t motorolaStartBit(uint16_t dbcStartBit) noexcept
{
    const uint16_t row = dbcStartBit >> 3;
    const uint16_t column = dbcStartBit & 0b111;
    return static_cast<uint16_t>((row * 8) + (7 - column));
}

} // namespace

class CanDbSignalTest : public QObject
{
    Q_OBJECT

private slots:
    void unsignedConversionAppliesFactorAndOffset();
    void negativeFactorAndOffset();

    void dbcStartBitRoundTrips_data();
    void dbcStartBitRoundTrips();

    void signExtension_data();
    void signExtension();

    void signExtensionAtFullWidth();
    void degenerateLengthFallsBackToUnsigned();

    void physicalRoundTripsThroughMessage_data();
    void physicalRoundTripsThroughMessage();

    void injectPhysicalClampsUnsignedAtZero();
    void injectPhysicalRoundsRatherThanTruncates();

    void muxedSignalIsPresentOnlyForMatchingMuxValue();
    void unmuxedSignalIsAlwaysPresent();

    void valueTableLookup();
};

void CanDbSignalTest::unsignedConversionAppliesFactorAndOffset()
{
    CanDbSignal sig(nullptr);
    sig.setLength(8);
    sig.setUnsigned(true);
    sig.setFactor(0.5);
    sig.setOffset(-40.0);

    QCOMPARE(sig.convertRawValueToPhysical(0), -40.0);
    QCOMPARE(sig.convertRawValueToPhysical(80), 0.0);
    QCOMPARE(sig.convertRawValueToPhysical(255), 87.5);
}

void CanDbSignalTest::negativeFactorAndOffset()
{
    CanDbSignal sig(nullptr);
    sig.setLength(8);
    sig.setUnsigned(true);
    sig.setFactor(-2.0);
    sig.setOffset(10.0);

    QCOMPARE(sig.convertRawValueToPhysical(0), 10.0);
    QCOMPARE(sig.convertRawValueToPhysical(5), 0.0);
}

void CanDbSignalTest::signExtension_data()
{
    QTest::addColumn<int>("length");
    QTest::addColumn<quint64>("raw");
    QTest::addColumn<double>("expected");

    // For a signed signal of n bits, raw values with the top bit set are negative.
    QTest::newRow("1bit -1")      << 1  << quint64(0x1) << -1.0;
    QTest::newRow("1bit 0")       << 1  << quint64(0x0) << 0.0;
    QTest::newRow("2bit -1")      << 2  << quint64(0x3) << -1.0;
    QTest::newRow("2bit -2")      << 2  << quint64(0x2) << -2.0;
    QTest::newRow("2bit 1")       << 2  << quint64(0x1) << 1.0;
    QTest::newRow("4bit -8")      << 4  << quint64(0x8) << -8.0;
    QTest::newRow("4bit 7")       << 4  << quint64(0x7) << 7.0;
    QTest::newRow("8bit -1")      << 8  << quint64(0xFF) << -1.0;
    QTest::newRow("8bit -128")    << 8  << quint64(0x80) << -128.0;
    QTest::newRow("8bit 127")     << 8  << quint64(0x7F) << 127.0;
    QTest::newRow("12bit -2048")  << 12 << quint64(0x800) << -2048.0;
    QTest::newRow("16bit -1")     << 16 << quint64(0xFFFF) << -1.0;
    QTest::newRow("16bit -32768") << 16 << quint64(0x8000) << -32768.0;
    QTest::newRow("32bit -1")     << 32 << quint64(0xFFFFFFFF) << -1.0;
    QTest::newRow("32bit min")    << 32 << quint64(0x80000000) << -2147483648.0;
    QTest::newRow("63bit -1")     << 63 << quint64(0x7FFFFFFFFFFFFFFFULL) << -1.0;
}

void CanDbSignalTest::signExtension()
{
    QFETCH(int, length);
    QFETCH(quint64, raw);
    QFETCH(double, expected);

    CanDbSignal sig(nullptr);
    sig.setLength(static_cast<uint16_t>(length));
    sig.setUnsigned(false);
    sig.setFactor(1.0);
    sig.setOffset(0.0);

    QCOMPARE(sig.convertRawValueToPhysical(raw), expected);

    // The same raw value read as unsigned must stay positive.
    sig.setUnsigned(true);
    QVERIFY(sig.convertRawValueToPhysical(raw) >= 0.0);
}

void CanDbSignalTest::signExtensionAtFullWidth()
{
    CanDbSignal sig(nullptr);
    sig.setLength(64);
    sig.setUnsigned(false);
    sig.setFactor(1.0);
    sig.setOffset(0.0);

    QCOMPARE(sig.convertRawValueToPhysical(0xFFFFFFFFFFFFFFFFULL), -1.0);
    QCOMPARE(sig.convertRawValueToPhysical(0x8000000000000000ULL),
             static_cast<double>(std::numeric_limits<int64_t>::min()));
    QCOMPARE(sig.convertRawValueToPhysical(1), 1.0);
}

// A DBC file's signal length is not validated, so 0 and >64 reach this code.
// Sign extension is impossible there (the shift width would be undefined), so
// the value must be read as unsigned instead of invoking undefined behaviour.
void CanDbSignalTest::degenerateLengthFallsBackToUnsigned()
{
    for (const uint16_t length : { uint16_t(0), uint16_t(65), uint16_t(100) })
    {
        CanDbSignal sig(nullptr);
        sig.setLength(length);
        sig.setUnsigned(false);
        sig.setFactor(1.0);
        sig.setOffset(0.0);

        QCOMPARE(sig.convertRawValueToPhysical(0), 0.0);
        QCOMPARE(sig.convertRawValueToPhysical(1), 1.0);
        QVERIFY(sig.convertRawValueToPhysical(0xFF) > 0.0);
    }
}

void CanDbSignalTest::physicalRoundTripsThroughMessage_data()
{
    QTest::addColumn<int>("dbcStartBit");
    QTest::addColumn<int>("length");
    QTest::addColumn<bool>("bigEndian");
    QTest::addColumn<bool>("isUnsigned");
    QTest::addColumn<double>("factor");
    QTest::addColumn<double>("offset");
    QTest::addColumn<double>("physical");

    QTest::newRow("intel u8 scaled")   << 0  << 8  << false << true  << 0.5  << -40.0 << 20.0;
    QTest::newRow("intel s16")         << 8  << 16 << false << false << 1.0  << 0.0   << -1234.0;
    QTest::newRow("motorola u12")      << 7  << 12 << true  << true  << 1.0  << 0.0   << 3000.0;
    QTest::newRow("motorola s12")      << 7  << 12 << true  << false << 1.0  << 0.0   << -2000.0;
    QTest::newRow("motorola unaligned")<< 11 << 4  << true  << true  << 1.0  << 0.0   << 9.0;
    QTest::newRow("motorola scaled")   << 23 << 16 << true  << true  << 0.01 << 0.0   << 12.34;
    QTest::newRow("intel s32 offset")  << 0  << 32 << false << false << 1.0  << 100.0 << -50.0;
}

// Encoding a physical value and decoding it again must return the same number.
void CanDbSignalTest::physicalRoundTripsThroughMessage()
{
    QFETCH(int, dbcStartBit);
    QFETCH(int, length);
    QFETCH(bool, bigEndian);
    QFETCH(bool, isUnsigned);
    QFETCH(double, factor);
    QFETCH(double, offset);
    QFETCH(double, physical);

    CanDbSignal sig(nullptr);
    sig.setStartBit(bigEndian ? motorolaStartBit(static_cast<uint16_t>(dbcStartBit))
                              : static_cast<uint16_t>(dbcStartBit));
    sig.setLength(static_cast<uint16_t>(length));
    sig.setIsBigEndian(bigEndian);
    sig.setUnsigned(isUnsigned);
    sig.setFactor(factor);
    sig.setOffset(offset);

    BusMessage msg;
    msg.setLength(8);
    sig.injectPhysicalIntoMessage(msg, physical);

    QCOMPARE(sig.extractPhysicalFromMessage(msg), physical);
}

// An unsigned signal cannot hold a negative value; it must saturate at zero
// rather than wrapping around to a huge positive number.
void CanDbSignalTest::injectPhysicalClampsUnsignedAtZero()
{
    CanDbSignal sig(nullptr);
    sig.setStartBit(0);
    sig.setLength(8);
    sig.setIsBigEndian(false);
    sig.setUnsigned(true);
    sig.setFactor(1.0);
    sig.setOffset(0.0);

    BusMessage msg;
    msg.setLength(8);
    sig.injectPhysicalIntoMessage(msg, -5.0);

    QCOMPARE(sig.extractRawDataFromMessage(msg), 0ULL);
}

void CanDbSignalTest::injectPhysicalRoundsRatherThanTruncates()
{
    CanDbSignal sig(nullptr);
    sig.setStartBit(0);
    sig.setLength(8);
    sig.setIsBigEndian(false);
    sig.setUnsigned(true);
    sig.setFactor(1.0);
    sig.setOffset(0.0);

    BusMessage msg;
    msg.setLength(8);

    sig.injectPhysicalIntoMessage(msg, 9.6);
    QCOMPARE(sig.extractRawDataFromMessage(msg), 10ULL);

    sig.injectPhysicalIntoMessage(msg, 9.4);
    QCOMPARE(sig.extractRawDataFromMessage(msg), 9ULL);
}

// isPresentInMessage gates whether a muxed signal is decoded at all. If it
// answers wrongly the trace shows a value decoded from unrelated payload bytes.
void CanDbSignalTest::muxedSignalIsPresentOnlyForMatchingMuxValue()
{
    CanDbMessage dbMsg(nullptr);
    dbMsg.setRaw_id(0x100);
    dbMsg.setDlc(8);

    auto *muxer = new CanDbSignal(&dbMsg);
    muxer->setName("Mux");
    muxer->setStartBit(0);
    muxer->setLength(4);
    muxer->setIsBigEndian(false);
    muxer->setUnsigned(true);
    muxer->setIsMuxer(true);
    dbMsg.addSignal(muxer);
    // addSignal does not infer this; the parent's muxer pointer is what
    // isPresentInMessage() consults, and DbcParser sets it explicitly.
    dbMsg.setMuxer(muxer);

    auto *muxed = new CanDbSignal(&dbMsg);
    muxed->setName("Muxed");
    muxed->setStartBit(8);
    muxed->setLength(8);
    muxed->setIsBigEndian(false);
    muxed->setUnsigned(true);
    muxed->setIsMuxed(true);
    muxed->setMuxValue(2);
    dbMsg.addSignal(muxed);

    BusMessage msg;
    msg.setId(0x100);
    msg.setLength(8);

    muxer->injectRawSignalIntoMessage(msg, 2);
    QVERIFY(muxed->isPresentInMessage(msg));

    muxer->injectRawSignalIntoMessage(msg, 3);
    QVERIFY(!muxed->isPresentInMessage(msg));

    muxer->injectRawSignalIntoMessage(msg, 0);
    QVERIFY(!muxed->isPresentInMessage(msg));
}

void CanDbSignalTest::unmuxedSignalIsAlwaysPresent()
{
    CanDbMessage dbMsg(nullptr);
    dbMsg.setRaw_id(0x200);
    dbMsg.setDlc(8);

    auto *sig = new CanDbSignal(&dbMsg);
    sig->setStartBit(0);
    sig->setLength(8);
    sig->setIsBigEndian(false);
    dbMsg.addSignal(sig);

    BusMessage msg;
    msg.setId(0x200);
    msg.setLength(8);

    QVERIFY(sig->isPresentInMessage(msg));
}

void CanDbSignalTest::valueTableLookup()
{
    CanDbSignal sig(nullptr);
    sig.setLength(4);
    sig.setValueName(0, "Off");
    sig.setValueName(1, "On");
    sig.setValueName(15, "Invalid");

    QCOMPARE(sig.getValueName(0), QString("Off"));
    QCOMPARE(sig.getValueName(1), QString("On"));
    QCOMPARE(sig.getValueName(15), QString("Invalid"));
    QVERIFY(sig.getValueName(7).isEmpty());
}

// Issue #42: the internal start bit differs from both the DBC value and the
// Intel-style LSB position Vector shows. All three describe the same bits.
void CanDbSignalTest::dbcStartBitRoundTrips_data()
{
    QTest::addColumn<int>("dbcStart");
    QTest::addColumn<int>("length");
    QTest::addColumn<int>("expectedLsb");

    // ThrottlePos 24|8@0+ from the issue: Vector shows 33.
    QTest::newRow("24|8")  << 24 << 8  << 33;
    QTest::newRow("7|16")  << 7  << 16 << 8;
    QTest::newRow("23|8")  << 23 << 8  << 16;
    QTest::newRow("0|16")  << 0  << 16 << 17;
    QTest::newRow("7|1")   << 7  << 1  << 7;
}

void CanDbSignalTest::dbcStartBitRoundTrips()
{
    QFETCH(int, dbcStart);
    QFETCH(int, length);
    QFETCH(int, expectedLsb);

    CanDbSignal sig(nullptr);
    sig.setIsBigEndian(true);
    sig.setStartBit(motorolaStartBit(static_cast<uint16_t>(dbcStart)));
    sig.setLength(static_cast<uint16_t>(length));

    QCOMPARE(sig.dbcStartBit(), static_cast<uint16_t>(dbcStart));
    QCOMPARE(sig.lsbBitIndex(), static_cast<uint16_t>(expectedLsb));

    sig.setIsBigEndian(false);
    sig.setStartBit(static_cast<uint16_t>(dbcStart));
    QCOMPARE(sig.dbcStartBit(), static_cast<uint16_t>(dbcStart));
    QCOMPARE(sig.lsbBitIndex(), static_cast<uint16_t>(dbcStart));
}

QTEST_APPLESS_MAIN(CanDbSignalTest)

#include "CanDbSignalTest.moc"
