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

/*

  Copyright (c) 2022 Ethan Zonca
  Copyright (c) 2024 CANgaroo Contributors

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

#include <array>
#include <atomic>
#include <memory>

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>

#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"
#include "driver/SerialTransport/SerialTransport.h"

class SLCANDriver;

class SLCANInterface : public BusInterface
{
    Q_OBJECT

public:
    enum class Manufacturer : uint32_t
    {
        CANable,
        WeActStudio,
        Elmue,  // CANable 2.5 Slcan firmware by ElmueSoft (any board, CANable 2 USB ID)
    };

    // `transportFactory` makes the port each open() talks to: a real serial
    // port from SLCANDriver, a simulated device in tests.
    SLCANInterface(SLCANDriver *driver, int index, QString name, bool fdSupport, Manufacturer manufacturer,
                   SerialTransportFactory transportFactory);
    ~SLCANInterface() override;

    QString getName() const override;
    void setName(QString name);
    QString getDetailsStr() const override;
    QString getVersion() override;
    int getIfIndex() const noexcept;

    QList<CanTiming> getAvailableBitrates() override;
    void applyConfig(const MeasurementInterface &mi) override;
    unsigned getBitrate() override;
    uint32_t getCapabilities() override;

    void open() override;
    void close() override;
    bool isOpen() override;

    // Opens the port just long enough to read the firmware version (and with
    // it whether this is ElmueSoft firmware), so version, capabilities and
    // bitrates are right before the first open(). For discovery; false if the
    // port cannot be opened or nothing answered. Leaves the CAN channel closed.
    bool probe();

    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    bool updateStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames() override;
    int getNumRxErrors() override;
    int getNumRxOverruns() override;
    int getNumTxFrames() override;
    int getNumTxErrors() override;
    int getNumTxDropped() override;

private:

    struct TxEntry
    {
        QByteArray frame;
        BusMessage msg;
    };

    void writePort(const char *cmd);
    void writePort(const QByteArray &data);
    // A setup command. A confirming device answers it with CR (done) or BEL
    // (refused); false and a warning if it refused or did not answer.
    bool sendConfig(const QByteArray &cmd);
    // ElmueSoft firmware, known from the USB strings or the "V" reply
    [[nodiscard]] bool isElmue() const;
    void queryDevice();
    // Elmue: "s"/"y" for the configured bitrate and sample point, from the
    // clock and limits its "V" reply gave. Empty if they are unknown or the
    // bitrate cannot be made; the "S"/"Y" presets are used then.
    [[nodiscard]] QByteArray elmueTimingCommand(bool data) const;
    void configureBitrate();
    void configureFdBitrate();
    void drainTxQueue(QList<BusMessage> &msglist);
    void handleTxConfirm(QList<BusMessage> &msglist, bool success);
    bool parseRxLine(QList<BusMessage> &msglist);
    // Marks the interface offline if the serial port failed or was unplugged.
    bool checkPortLost();
    [[nodiscard]] static QByteArray encodeFrame(const BusMessage &msg);

    Manufacturer _manufacturer;
    int _idx;
    QString _name;
    QString _version;
    bool _fdSupport{false};
    MeasurementInterface _settings;

    SerialTransportFactory _transportFactory;
    std::unique_ptr<SerialTransport> _port;
    QByteArray _rxLineBuffer;

    // Frames awaiting device ACK — only accessed from BusListener thread
    QList<BusMessage> _txPendingConfirm;

    // TX queue — written by main thread, drained by BusListener thread
    mutable QMutex _txMutex;
    QList<TxEntry> _txQueue;

    std::atomic<bool> _isOpen{false};
    std::atomic<bool> _isOffline{false};
    // True when device does not echo CR/BEL confirmations for TX frames
    std::atomic<bool> _noConfirm{false};
    // "V" answered like the ElmueSoft firmware (also where the USB strings
    // that SLCANDriver uses are not available, e.g. Windows)
    std::atomic<bool> _elmueFirmware{false};
    // From Elmue's "V" reply: CAN clock and nominal / data timing limits
    uint32_t _elmueClockHz{0};
    std::array<uint32_t, 8> _elmueLimits{};

    std::atomic<uint32_t> _state{state_bus_off};
    std::atomic<uint64_t> _rxCount{0};
    std::atomic<int>      _rxErrors{0};
    std::atomic<uint64_t> _rxOverruns{0};
    std::atomic<uint64_t> _txCount{0};
    std::atomic<int>      _txErrors{0};
    std::atomic<uint64_t> _txDropped{0};
};
