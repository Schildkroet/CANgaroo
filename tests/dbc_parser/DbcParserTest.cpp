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

// DbcParser, end to end: DBC text in, populated CanDb out.
//
// The most important case here is the Motorola start-bit conversion. BusMessage
// receives an already-converted start bit, so the packing tests in
// bus_message_signal only pin the second half of that contract -- this file pins
// the parser side, which is where the issue #34 convention actually lives.

#include <QtTest>

#include "db/model/CanDb.h"
#include "db/model/CanDbMessage.h"
#include "db/model/CanDbSignal.h"
#include "db/dbc/DbcParser.h"

namespace
{

const QString k_dbcPreamble = R"(VERSION "test-1.0"

NS_:

BS_:

BU_: ECU_A ECU_B

)";

} // namespace

class DbcParserTest : public QObject
{
    Q_OBJECT

private:
    // Parses DBC text via a temporary file (DbcParser works on QFile).
    bool parse(const QString &body, CanDb &db, bool withPreamble = true);

private slots:
    void parsesVersionAndMessage();
    void parsesSignalAttributes();

    void motorolaStartBitIsConverted_data();
    void motorolaStartBitIsConverted();

    void intelStartBitIsUnchanged();
    void byteOrderFlagMapping();
    void signedAndUnsignedFlag();

    void parsesExtendedIdentifier();

    void protocolTypeFlagsJ1939Database();
    void otherAttributesAreSkipped();
    void parsesMultiplexer();
    void parsesValueTable();
    void parsesComments();
    void parsesMultipleMessagesAndSignals();

    void unknownSectionsAreSkipped();
    void rejectsTruncatedSignal();
};

bool DbcParserTest::parse(const QString &body, CanDb &db, bool withPreamble)
{
    QTemporaryFile file;
    if (!file.open()) { return false; }

    {
        QTextStream out(&file);
        if (withPreamble) { out << k_dbcPreamble; }
        out << body;
    }
    file.flush();
    file.seek(0);

    DbcParser parser;
    return parser.parseFile(&file, db);
}

void DbcParserTest::parsesVersionAndMessage()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 256 TestMsg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", db));

    QCOMPARE(db.getVersion(), QString("test-1.0"));
    QCOMPARE(db.getNumberOfMessages(), size_t(1));

    CanDbMessage *msg = db.getMessageById(256);
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("TestMsg"));
    QCOMPARE(msg->getDlc(), uint8_t(8));
    QCOMPARE(msg->getSignals().size(), 1);
}

void DbcParserTest::parsesSignalAttributes()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Temp : 0|16@1- (0.03125,-273.15) [-273.15|1000] "degC" ECU_B
)", db));

    CanDbSignal *sig = db.getMessageById(100)->getSignalByName("Temp");
    QVERIFY(sig != nullptr);
    QCOMPARE(sig->startBit(), uint16_t(0));
    QCOMPARE(sig->length(), uint16_t(16));
    QCOMPARE(sig->getFactor(), 0.03125);
    QCOMPARE(sig->getOffset(), -273.15);
    QCOMPARE(sig->getMinimumValue(), -273.15);
    QCOMPARE(sig->getMaximumValue(), 1000.0);
    QCOMPARE(sig->getUnit(), QString("degC"));
    QVERIFY(!sig->isUnsigned());
    QVERIFY(!sig->isBigEndian());
}

void DbcParserTest::motorolaStartBitIsConverted_data()
{
    QTest::addColumn<int>("dbcStartBit");
    QTest::addColumn<int>("expectedInternal");

    // A Motorola start bit (the MSB position, in DBC "sawtooth" numbering) is
    // stored as a sequential MSB-first bit index: (byte * 8) + (7 - bit).
    QTest::newRow("7 -> 0")     << 7   << 0;
    QTest::newRow("0 -> 7")     << 0   << 7;
    QTest::newRow("11 -> 12")   << 11  << 12;
    QTest::newRow("23 -> 16")   << 23  << 16;
    QTest::newRow("18 -> 21")   << 18  << 21;
    QTest::newRow("39 -> 32")   << 39  << 32;
    QTest::newRow("52 -> 51")   << 52  << 51;
    QTest::newRow("63 -> 56")   << 63  << 56;
}

// This is the conversion that issue #34 turned on. If it changes, the packing
// tests in bus_message_signal are measuring the wrong thing.
void DbcParserTest::motorolaStartBitIsConverted()
{
    QFETCH(int, dbcStartBit);
    QFETCH(int, expectedInternal);

    CanDb db;
    QVERIFY(parse(QString(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : %1|1@0+ (1,0) [0|1] "" ECU_B
)").arg(dbcStartBit), db));

    CanDbSignal *sig = db.getMessageById(100)->getSignalByName("Sig");
    QVERIFY(sig != nullptr);
    QVERIFY(sig->isBigEndian());
    QCOMPARE(sig->startBit(), static_cast<uint16_t>(expectedInternal));
}

// Intel signals are stored exactly as written.
void DbcParserTest::intelStartBitIsUnchanged()
{
    for (const int startBit : { 0, 1, 7, 11, 32, 63 })
    {
        CanDb db;
        QVERIFY(parse(QString(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : %1|1@1+ (1,0) [0|1] "" ECU_B
)").arg(startBit), db));

        CanDbSignal *sig = db.getMessageById(100)->getSignalByName("Sig");
        QVERIFY(sig != nullptr);
        QVERIFY(!sig->isBigEndian());
        QCOMPARE(sig->startBit(), static_cast<uint16_t>(startBit));
    }
}

// @0 is Motorola/big-endian, @1 is Intel/little-endian -- easy to invert.
void DbcParserTest::byteOrderFlagMapping()
{
    CanDb bigEndian;
    QVERIFY(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 7|8@0+ (1,0) [0|255] "" ECU_B
)", bigEndian));
    QVERIFY(bigEndian.getMessageById(100)->getSignalByName("Sig")->isBigEndian());

    CanDb littleEndian;
    QVERIFY(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", littleEndian));
    QVERIFY(!littleEndian.getMessageById(100)->getSignalByName("Sig")->isBigEndian());
}

void DbcParserTest::signedAndUnsignedFlag()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ U : 0|8@1+ (1,0) [0|255] "" ECU_B
 SG_ S : 8|8@1- (1,0) [-128|127] "" ECU_B
)", db));

    CanDbMessage *msg = db.getMessageById(100);
    QVERIFY(msg->getSignalByName("U")->isUnsigned());
    QVERIFY(!msg->getSignalByName("S")->isUnsigned());
}

// DBC stores extended ids with bit 31 set; the raw id keeps that marker.
void DbcParserTest::parsesExtendedIdentifier()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 2566844926 ExtMsg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", db));

    CanDbMessage *msg = db.getMessageById(2566844926u);
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("ExtMsg"));
    QVERIFY((msg->getRaw_id() & 0x80000000u) != 0);
}

void DbcParserTest::protocolTypeFlagsJ1939Database()
{
    CanDb plain;
    QVERIFY(parse(R"(BO_ 100 Msg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", plain));
    QVERIFY(!plain.isJ1939());

    CanDb j1939;
    QVERIFY(parse(R"(BO_ 2566844926 Msg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B

BA_DEF_ "ProtocolType" STRING ;
BA_DEF_DEF_ "ProtocolType" "";
BA_ "ProtocolType" "J1939";
)", j1939));
    QVERIFY(j1939.isJ1939());
    QVERIFY(j1939.getMessageById(2566844926u) != nullptr);
}

void DbcParserTest::otherAttributesAreSkipped()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 Msg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B

BA_ "BusType" "CAN";
BA_ "GenMsgCycleTime" BO_ 100 50;
BA_ "ProtocolType" "CAN";
)", db));
    QVERIFY(!db.isJ1939());
    QVERIFY(db.getMessageById(100) != nullptr);
}

void DbcParserTest::parsesMultiplexer()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 MuxMsg: 8 ECU_A
 SG_ Mode M : 0|4@1+ (1,0) [0|15] "" ECU_B
 SG_ ValueA m0 : 8|8@1+ (1,0) [0|255] "" ECU_B
 SG_ ValueB m1 : 8|8@1+ (1,0) [0|255] "" ECU_B
)", db));

    CanDbMessage *msg = db.getMessageById(100);
    QVERIFY(msg != nullptr);

    CanDbSignal *mode = msg->getSignalByName("Mode");
    QVERIFY(mode->isMuxer());
    QVERIFY(!mode->isMuxed());
    // The parent's muxer pointer must be wired up, otherwise every muxed signal
    // is treated as absent and silently disappears from the trace.
    QCOMPARE(msg->getMuxer(), mode);

    CanDbSignal *a = msg->getSignalByName("ValueA");
    QVERIFY(a->isMuxed());
    QVERIFY(!a->isMuxer());
    QCOMPARE(a->getMuxValue(), 0u);

    CanDbSignal *b = msg->getSignalByName("ValueB");
    QVERIFY(b->isMuxed());
    QCOMPARE(b->getMuxValue(), 1u);
}

void DbcParserTest::parsesValueTable()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ State : 0|4@1+ (1,0) [0|15] "" ECU_B

VAL_ 100 State 0 "Off" 1 "On" 15 "Invalid" ;
)", db));

    CanDbSignal *sig = db.getMessageById(100)->getSignalByName("State");
    QCOMPARE(sig->getValueName(0), QString("Off"));
    QCOMPARE(sig->getValueName(1), QString("On"));
    QCOMPARE(sig->getValueName(15), QString("Invalid"));
    QVERIFY(sig->getValueName(7).isEmpty());
}

void DbcParserTest::parsesComments()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B

CM_ BO_ 100 "message comment";
CM_ SG_ 100 Sig "signal comment";
)", db));

    CanDbMessage *msg = db.getMessageById(100);
    QCOMPARE(msg->getComment(), QString("message comment"));
    QCOMPARE(msg->getSignalByName("Sig")->comment(), QString("signal comment"));
}

void DbcParserTest::parsesMultipleMessagesAndSignals()
{
    CanDb db;
    QVERIFY(parse(R"(BO_ 100 First: 8 ECU_A
 SG_ A : 0|8@1+ (1,0) [0|255] "" ECU_B
 SG_ B : 8|8@1+ (1,0) [0|255] "" ECU_B

BO_ 200 Second: 4 ECU_B
 SG_ C : 0|16@0+ (1,0) [0|65535] "" ECU_A
)", db));

    QCOMPARE(db.getNumberOfMessages(), size_t(2));
    QCOMPARE(db.getMessageById(100)->getSignals().size(), 2);
    QCOMPARE(db.getMessageById(200)->getSignals().size(), 1);
    QCOMPARE(db.getMessageById(200)->getName(), QString("Second"));
    QCOMPARE(db.getMessageById(200)->getDlc(), uint8_t(4));
}

// The parser is deliberately lenient about sections it does not implement
// (BA_, SIG_VALTYPE_, ...): they are skipped, not rejected. The consequence is
// worth knowing -- a file that is not a DBC at all parses "successfully" and
// simply yields no messages, so callers must check the message count rather than
// trust the return value alone.
void DbcParserTest::unknownSectionsAreSkipped()
{
    CanDb unknownOnly;
    QVERIFY(parse("this is not a DBC file at all\n", unknownOnly, false));
    QCOMPARE(unknownOnly.getNumberOfMessages(), size_t(0));

    // An unimplemented section between real ones must not disturb them.
    CanDb mixed;
    QVERIFY(parse(R"(BO_ 100 First: 8 ECU_A
 SG_ A : 0|8@1+ (1,0) [0|255] "" ECU_B

BA_DEF_ SG_ "GenSigStartValue" INT 0 10000;

BO_ 200 Second: 8 ECU_A
 SG_ B : 0|8@1+ (1,0) [0|255] "" ECU_B
)", mixed));

    QCOMPARE(mixed.getNumberOfMessages(), size_t(2));
    QVERIFY(mixed.getMessageById(100) != nullptr);
    QVERIFY(mixed.getMessageById(200) != nullptr);
}

// A signal line missing its trailing fields must fail rather than yield a
// half-populated signal.
void DbcParserTest::rejectsTruncatedSignal()
{
    CanDb db;
    QVERIFY(!parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 0|8@1+
)", db));
}

QTEST_APPLESS_MAIN(DbcParserTest)

#include "DbcParserTest.moc"
