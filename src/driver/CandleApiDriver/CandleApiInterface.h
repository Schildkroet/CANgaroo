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

#ifndef CANDLEAPIINTERFACE_H
#define CANDLEAPIINTERFACE_H

#include "driver/BusInterface.h"
#include "core/MeasurementInterface.h"
#include "CandleSharedDevice.h"

#include <QList>
#include <QMutex>

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

#include "CandleApiTiming.h"
#include "api/candle.h"

class CandleApiDriver;

class CandleApiInterface : public BusInterface
{
    Q_OBJECT
public:
    CandleApiInterface(CandleApiDriver *driver,
                       std::shared_ptr<CandleSharedDevice> sharedDev,
                       uint8_t channel);
    ~CandleApiInterface() override;

    QString getName() const override;
    QString getDetailsStr() const override;

    void applyConfig(const MeasurementInterface &mi) override;

    unsigned getBitrate() override;

    uint32_t getCapabilities() override;
    QList<CanTiming> getAvailableBitrates() override;

    void open() override;
    bool isOpen() override;
    void close() override;

    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    bool updateStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames() override;
    int getNumRxErrors() override;
    int getNumTxFrames() override;
    int getNumTxErrors() override;
    int getNumRxOverruns() override;
    int getNumTxDropped() override;

    QString getVersion();
    std::wstring getPath() const;
    uint8_t getChannel() const;

private:

    uint8_t _channel;
    std::atomic<bool> _isOpen;
    std::atomic<bool> _isFdEnabled;
    std::atomic<bool> _getStateFailed{false};

    std::shared_ptr<CandleSharedDevice> _sharedDev;
    MeasurementInterface _settings;
    Backend &_backend;

    uint64_t _numRx;
    uint64_t _numTx;
    uint64_t _numTxErr;
    uint64_t _numRxOverruns;

    QList<CandleApiTiming> _timings;
    QList<CandleApiTiming> _fdTimings;

    bool setBitTiming(uint32_t bitrate, uint32_t samplePoint);
    bool setDataBitTiming(uint32_t bitrate, uint32_t samplePoint);

    // Frames handed to the device, waiting for the echo that confirms they were
    // transmitted on the bus; only then are they shown as TX.
    static constexpr int MaxPendingTx = 256;
    QMutex _txMutex;
    QList<BusMessage> _pendingTx;

    bool takeConfirmedTx(const candle_fd_frame_t &echo, BusMessage &txMsg);
};

#endif // CANDLEAPIINTERFACE_H
