/*

  Copyright (c) 2024 - 2026 Schildkroet

  This file is part of CANgaroo.

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

#include "../BusInterface.h"

#include <atomic>

#include <QtSerialPort/QSerialPort>

#include "core/MeasurementInterface.h"

#include "GrIP/GrIPHandler.h"


class GrIPDriver;

struct can_config_t
{
    bool supports_canfd;
    bool supports_timing;
    uint32_t state;
    uint32_t base_freq;
    uint32_t sample_point;
    uint32_t ctrl_mode;
    uint32_t restart_ms;
};

struct can_status_t
{
    std::atomic<uint32_t> can_state{0};

    std::atomic<uint64_t> rx_count{0};
    std::atomic<int> rx_errors{0};
    std::atomic<uint64_t> rx_overruns{0};

    std::atomic<uint64_t> tx_count{0};
    std::atomic<int> tx_errors{0};
    std::atomic<uint64_t> tx_dropped{0};
};

class GrIPInterface : public BusInterface
{
    Q_OBJECT

public:
    enum
    {
        CANIL_CAN,
        CANIL_LIN
    };

    GrIPInterface(GrIPDriver *driver, int index, int channel_idx, GrIPHandler *hdl, QString name, bool fd_support, uint32_t manufacturer);
    ~GrIPInterface() override;

    QString getDetailsStr() const override;
    QString getName() const override;
    void setName(QString name);

    QList<CanTiming> getAvailableBitrates() override;

    void applyConfig(const MeasurementInterface &mi) override;
    bool readConfig();
    bool readConfigFromLink(struct rtnl_link *link);

    bool supportsTimingConfiguration();
    bool supportsCanFD();
    bool supportsTripleSampling();

    unsigned getBitrate() override;
    BusType busType() const override;
    uint32_t getCapabilities() override;

    void open() override;
    void close() override;
    bool isOpen() override;

    void sendLinSleepWakeup(bool wakeup) override;
    void setLinScheduleTable(uint8_t tableIndex) override;
    void sendLinDiagRequest(uint8_t nad, const uint8_t *data, uint8_t len) override;
    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    bool updateStatistics() override;
    void resetStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames() override;
    int getNumRxErrors() override;
    int getNumRxOverruns() override;

    int getNumTxFrames() override;
    int getNumTxErrors() override;
    int getNumTxDropped() override;

    QString getVersion() override;

    // Asks the device for this channel's capabilities (blocks ~50 ms). open()
    // does it too; discovery calls it so they are right before the first open.
    void refreshCapabilities();

    int getIfIndex();
    GrIPHandler *handler() const { return m_GrIPHandler; }

private slots:
    void handleSerialError(QSerialPort::SerialPortError error);

private:
    bool updateStatus();

    uint32_t _manufacturer;
    QString _version;

    int _idx;
    int _channel_idx;
    std::atomic<bool> _isOpen{false};
    std::atomic<bool> _isOffline{false};
    bool _isLin;
    QString _name;

    MeasurementInterface _settings;

    can_config_t _config;
    can_status_t _status;

    qint64 _lastStateMsec; ///< Timestamp of last open(); used to auto-clear error state after 3 s.
    qint64 _lastReadMsec;  ///< Timestamp of last readMessage() execution; used for rate-limiting.

    GrIPHandler *m_GrIPHandler;

    uint32_t _deviceCaps{0};
};
