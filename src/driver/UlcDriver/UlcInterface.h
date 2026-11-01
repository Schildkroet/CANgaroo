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

#pragma once

#include "driver/BusInterface.h"
#include "core/MeasurementInterface.h"

#include <atomic>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QtSerialPort/QSerialPort>

class UlcDriver;
class LinDb;

// uCAN USB LIN Converter (uLC): LIN master / slave / monitor behind a USB CDC
// virtual COM port, driven with an SLCAN-style ASCII protocol (UlcFrameCodec.h).
class UlcInterface : public BusInterface
{
    Q_OBJECT

public:
    UlcInterface(UlcDriver *driver, int index, QString portName);
    ~UlcInterface() override;

    QString getName() const override;
    void setName(QString portName);
    QString getDetailsStr() const override;
    QString getVersion() override;
    BusType busType() const override { return BusType::LIN; }
    int getIfIndex() const noexcept;

    QList<CanTiming> getAvailableBitrates() override;
    void applyConfig(const MeasurementInterface &mi) override;
    unsigned getBitrate() override;
    uint32_t getCapabilities() override;

    void open() override;
    void close() override;
    bool isOpen() override;

    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    void setLinScheduleTable(uint8_t tableIndex) override;

    bool updateStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames() override;
    int getNumRxErrors() override;
    int getNumRxOverruns() override;
    int getNumTxFrames() override;
    int getNumTxErrors() override;
    int getNumTxDropped() override;

private:
    enum class Mode { Monitor, Master, Slave };

    enum class TxKind
    {
        OneShot,        // monitor mode: frame goes out once, shown in the trace when acknowledged
        PayloadUpdate,  // master/slave: new data for a slot this node publishes
        Control,        // schedule commands queued by setLinScheduleTable()
    };

    struct TxEntry
    {
        QByteArray cmd;
        TxKind kind;
        BusMessage msg;
    };

    // Writes one command during open() and waits for its reply line, skipping
    // bus frames that may still arrive. Returns the reply without CR, "\a" for
    // BEL, or nothing on timeout.
    std::optional<QByteArray> transact(const QByteArray &cmd, int timeout_ms = 200);
    bool loadLdf(LinDb &ldb) const;
    // Slot commands for one LDF schedule table / for the selected slave node,
    // in upload order. `published` receives the IDs this node transmits.
    QList<QByteArray> masterTableCommands(const LinDb &ldb, int tableIndex,
                                          QSet<uint8_t> &published) const;
    QList<QByteArray> slaveTableCommands(const LinDb &ldb, QSet<uint8_t> &published) const;
    QByteArray defaultPayload(uint8_t id, uint8_t len) const;
    void drainTxQueue();
    void handleLine(QList<BusMessage> &msglist);
    void handleTxReply(QList<BusMessage> &msglist, bool success);

    int _idx;
    QString _name;
    QString _version;
    MeasurementInterface _settings;
    Mode _mode{Mode::Monitor};

    std::unique_ptr<QSerialPort> _port;
    QByteArray _rxLineBuffer;

    // Frame IDs whose payload this node publishes in the running table. Only
    // these may be updated by sendMessage(): for any other ID the firmware
    // would turn a subscriber row into a publisher or append a new slot.
    // Guarded by _txMutex: setLinScheduleTable() changes it from the GUI thread.
    QSet<uint8_t> _publishedIds;
    // Last payload sent per published ID, reused when the schedule is reloaded (_txMutex)
    QMap<uint8_t, QByteArray> _sentPayloads;

    // Commands sent but not answered yet -- only accessed from BusListener thread
    QList<TxEntry> _txPendingReply;
    // ID of the empty echo expected after a one-shot frame, -1 if none (BusListener thread)
    int _swallowEmptyId{-1};

    // TX queue -- written by main thread, drained by BusListener thread
    mutable QMutex _txMutex;
    QList<TxEntry> _txQueue;

    std::atomic<bool> _isOpen{false};
    std::atomic<bool> _isOffline{false};

    std::atomic<uint32_t> _state{state_stopped};
    std::atomic<uint64_t> _rxCount{0};
    std::atomic<int>      _rxErrors{0};
    std::atomic<uint64_t> _txCount{0};
    std::atomic<int>      _txErrors{0};
    std::atomic<uint64_t> _txDropped{0};
};
