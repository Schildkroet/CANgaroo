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

// CanDb container behaviour, in particular CanDb::updateFrom().
//
// updateFrom() is how a DBC reload lands in a database that the measurement setup
// already points at (MeasurementNetwork::reloadCanDbs), so it must reproduce
// everything the parser establishes on a fresh load -- not just the scalar signal
// attributes. It previously copied the per-signal isMuxer flag but never
// re-established the parent message's muxer pointer, which is what decides
// whether muxed signals are decoded at all.

#include <QtTest>

#include "core/BusMessage.h"
#include "db/model/CanDb.h"
#include "db/model/CanDbMessage.h"
#include "db/model/CanDbSignal.h"
#include "db/dbc/DbcParser.h"

namespace
{

const QString k_preamble = R"(VERSION "1.0"

NS_:

BS_:

BU_: ECU_A ECU_B

)";

const QString k_muxDbc = R"(BO_ 100 MuxMsg: 8 ECU_A
 SG_ Mode M : 0|4@1+ (1,0) [0|15] "" ECU_B
 SG_ ValueA m0 : 8|8@1+ (1,0) [0|255] "" ECU_B
 SG_ ValueB m1 : 8|8@1+ (1,0) [0|255] "" ECU_B
)";

const QString k_plainDbc = R"(BO_ 100 MuxMsg: 8 ECU_A
 SG_ Mode : 0|4@1+ (1,0) [0|15] "" ECU_B
 SG_ ValueA : 8|8@1+ (1,0) [0|255] "" ECU_B
)";

[[nodiscard]] bool parseInto(const QString &body, CanDb &db)
{
    QTemporaryFile file;
    if (!file.open()) { return false; }

    {
        QTextStream out(&file);
        out << k_preamble << body;
    }
    file.flush();
    file.seek(0);

    DbcParser parser;
    return parser.parseFile(&file, db);
}

// Builds the frame a muxed signal needs in order to be considered present.
[[nodiscard]] BusMessage muxMessage(uint32_t rawId, uint8_t mode)
{
    BusMessage msg;
    msg.setId(rawId);
    msg.setLength(8);
    msg.setByte(0, mode);
    return msg;
}

} // namespace

class CanDbTest : public QObject
{
    Q_OBJECT

private slots:
    void updateFromCopiesMessagesIntoEmptyDatabase();
    void updateFromCopiesSignalAttributes();
    void updateFromCopiesJ1939Flag();
    void updateFromWiresUpMuxerOnFirstLoad();
    void updateFromWiresUpNewlyAddedMuxer();
    void updateFromClearsMuxerWhenItDisappears();
    void updateFromKeepsMuxedSignalsDecodable();
    void updateFromReusesExistingSignalObjects();
    void getMessageByIdReturnsNullForUnknownId();
};

void CanDbTest::updateFromCopiesMessagesIntoEmptyDatabase()
{
    CanDb source;
    QVERIFY(parseInto(k_muxDbc, source));

    CanDb target;
    target.updateFrom(&source);

    QCOMPARE(target.getNumberOfMessages(), size_t(1));
    CanDbMessage *msg = target.getMessageById(100);
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("MuxMsg"));
    QCOMPARE(msg->getDlc(), uint8_t(8));
    QCOMPARE(msg->getSignals().size(), 3);
}

void CanDbTest::updateFromCopiesJ1939Flag()
{
    CanDb source;
    QVERIFY(parseInto(R"(BO_ 2566844926 M: 8 ECU_A
 SG_ S : 0|8@1+ (1,0) [0|255] "" ECU_B

BA_ "ProtocolType" "J1939";
)", source));
    QVERIFY(source.isJ1939());

    CanDb target;
    target.updateFrom(&source);
    QVERIFY(target.isJ1939());

    // A reload from a DBC without the attribute clears it again.
    CanDb plain;
    QVERIFY(parseInto(k_muxDbc, plain));
    target.updateFrom(&plain);
    QVERIFY(!target.isJ1939());
}

void CanDbTest::updateFromCopiesSignalAttributes()
{
    CanDb source;
    QVERIFY(parseInto(R"(BO_ 200 M: 8 ECU_A
 SG_ Temp : 7|16@0- (0.1,-40) [-40|100] "degC" ECU_B
)", source));

    CanDb target;
    target.updateFrom(&source);

    CanDbSignal *sig = target.getMessageById(200)->getSignalByName("Temp");
    QVERIFY(sig != nullptr);
    QCOMPARE(sig->startBit(), source.getMessageById(200)->getSignalByName("Temp")->startBit());
    QCOMPARE(sig->length(), uint16_t(16));
    QCOMPARE(sig->getFactor(), 0.1);
    QCOMPARE(sig->getOffset(), -40.0);
    QCOMPARE(sig->getUnit(), QString("degC"));
    QVERIFY(sig->isBigEndian());
    QVERIFY(!sig->isUnsigned());
}

// Regression: the muxer pointer must be established, not just the isMuxer flag.
void CanDbTest::updateFromWiresUpMuxerOnFirstLoad()
{
    CanDb source;
    QVERIFY(parseInto(k_muxDbc, source));

    CanDb target;
    target.updateFrom(&source);

    CanDbMessage *msg = target.getMessageById(100);
    CanDbSignal *mode = msg->getSignalByName("Mode");
    QVERIFY(mode != nullptr);
    QVERIFY(mode->isMuxer());
    QCOMPARE(msg->getMuxer(), mode);
}

// A reload where the DBC gained a multiplexer: the message object is reused, so
// the muxer pointer has to be attached to the reused signal.
void CanDbTest::updateFromWiresUpNewlyAddedMuxer()
{
    CanDb target;
    QVERIFY(parseInto(k_plainDbc, target));
    QVERIFY(target.getMessageById(100)->getMuxer() == nullptr);

    CanDb reloaded;
    QVERIFY(parseInto(k_muxDbc, reloaded));
    target.updateFrom(&reloaded);

    CanDbMessage *msg = target.getMessageById(100);
    CanDbSignal *mode = msg->getSignalByName("Mode");
    QVERIFY(mode->isMuxer());
    QCOMPARE(msg->getMuxer(), mode);
}

// The reverse: a reload that removed the multiplexer must not leave the message
// pointing at a signal that is no longer a muxer.
void CanDbTest::updateFromClearsMuxerWhenItDisappears()
{
    CanDb target;
    QVERIFY(parseInto(k_muxDbc, target));
    QVERIFY(target.getMessageById(100)->getMuxer() != nullptr);

    CanDb reloaded;
    QVERIFY(parseInto(k_plainDbc, reloaded));
    target.updateFrom(&reloaded);

    CanDbMessage *msg = target.getMessageById(100);
    QVERIFY(!msg->getSignalByName("Mode")->isMuxer());
    QVERIFY(msg->getMuxer() == nullptr);
}

// The user-visible consequence: without the muxer pointer every muxed signal is
// reported absent and silently vanishes from the trace after a reload.
void CanDbTest::updateFromKeepsMuxedSignalsDecodable()
{
    CanDb source;
    QVERIFY(parseInto(k_muxDbc, source));

    CanDb target;
    target.updateFrom(&source);

    CanDbMessage *msg = target.getMessageById(100);
    CanDbSignal *valueA = msg->getSignalByName("ValueA");
    CanDbSignal *valueB = msg->getSignalByName("ValueB");
    QVERIFY(valueA->isMuxed());
    QVERIFY(valueB->isMuxed());

    const BusMessage mode0 = muxMessage(100, 0);
    QVERIFY(valueA->isPresentInMessage(mode0));
    QVERIFY(!valueB->isPresentInMessage(mode0));

    const BusMessage mode1 = muxMessage(100, 1);
    QVERIFY(!valueA->isPresentInMessage(mode1));
    QVERIFY(valueB->isPresentInMessage(mode1));
}

// Signals are updated in place rather than replaced, which is what lets the
// measurement setup keep raw pointers to them across a reload.
void CanDbTest::updateFromReusesExistingSignalObjects()
{
    CanDb target;
    QVERIFY(parseInto(k_muxDbc, target));

    CanDbMessage *msg = target.getMessageById(100);
    CanDbSignal *before = msg->getSignalByName("ValueA");
    const int signalCount = msg->getSignals().size();

    CanDb reloaded;
    QVERIFY(parseInto(k_muxDbc, reloaded));
    target.updateFrom(&reloaded);

    QCOMPARE(target.getMessageById(100)->getSignalByName("ValueA"), before);
    QCOMPARE(target.getMessageById(100)->getSignals().size(), signalCount);
}

void CanDbTest::getMessageByIdReturnsNullForUnknownId()
{
    CanDb db;
    QVERIFY(parseInto(k_muxDbc, db));

    QVERIFY(db.getMessageById(0x999) == nullptr);
}

QTEST_APPLESS_MAIN(CanDbTest)

#include "CanDbTest.moc"
