/*

  Copyright (c) 2015, 2016 Hubert Denkmair
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
#pragma once

#include "../BusInterface.h"

#include <string>

#include <QMutex>
#include <atomic>

#include <linux/can/netlink.h>


class SocketCanDriver;

struct can_config_t
{
    bool supports_canfd;
    bool supports_timing;
    uint32_t state;
    uint32_t base_freq;
    uint32_t sample_point;
    uint32_t ctrl_mode;
    uint32_t restart_ms;
    struct can_bittiming bit_timing;
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

// Snapshot type for storing non-atomic offsets
struct can_status_snapshot_t
{
    uint32_t can_state{0};
    uint64_t rx_count{0};
    int rx_errors{0};
    uint64_t rx_overruns{0};
    uint64_t tx_count{0};
    int tx_errors{0};
    uint64_t tx_dropped{0};
};

class SocketCanInterface : public BusInterface
{
public:
    SocketCanInterface(SocketCanDriver *driver, int index, QString name);
    ~SocketCanInterface() override;

    QString getName() const override;
    void setName(QString name);

    QList<CanTiming> getAvailableBitrates() override;

    void applyConfig(const MeasurementInterface &mi) override;
    virtual bool readConfig();
    virtual bool readConfigFromLink(struct rtnl_link *link);

    bool supportsTimingConfiguration();
    bool supportsCanFD();
    bool supportsTripleSampling();

    QString getVersion() override;
    unsigned getBitrate() override;
    uint32_t getCapabilities() override;

    void open() override;
    bool isOpen() override;
    void close() override;

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

    int getIfIndex();

private:
    enum ts_mode_t
    {
        ts_mode_SIOCSHWTSTAMP,
        ts_mode_SIOCGSTAMPNS,
        ts_mode_SIOCGSTAMP
    };

    int _idx;
    bool _isOpen;
    bool _bitrateFallbackLogged = false;
    int _fd;
    QString _name;

    can_config_t _config;
    can_status_t _status;
    can_status_snapshot_t _offset_stats;
    ts_mode_t _ts_mode;

    const char *cname();
    std::string _cnameBuffer;
    bool updateStatus();

    QMutex _txMutex;
    QList<BusMessage> txMsgList;
    QMutex _fdMutex; // guards _fd and _isOpen

    QString buildIpRouteCmd(const MeasurementInterface &mi);
};
