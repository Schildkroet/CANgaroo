/*
  Copyright (c) 2025-2026 Schildkroet

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
#include "GrIPInterface.h"

#include <QDateTime>
#include <QDebug>
#include <QString>
#include <QStringList>
#include <QThread>

#include "core/Backend.h"
#include "core/Log.h"
#include "core/BusMessage.h"
#include "db/model/LinDb.h"
#include "core/MeasurementInterface.h"

#include "GrIP/GrIPHandler.h"




GrIPInterface::GrIPInterface(GrIPDriver *driver, int index, int channel_idx, GrIPHandler *hdl, QString name, bool fd_support, uint32_t manufacturer)
    : BusInterface(reinterpret_cast<CanDriver *>(driver)),
      _manufacturer(manufacturer),
      _idx(index),
      _channel_idx(channel_idx),
      _isLin(manufacturer == CANIL_LIN),
      _name(name),
      m_GrIPHandler(hdl)
{
    _settings.setBitrate(500000);
    _settings.setSamplePoint(875);

    _config.supports_canfd = fd_support;
    _config.supports_timing = false;
    //qDebug() << (_isLin ? "LIN IDX: " : "CAN IDX: ") << _channel_idx;

    if (_isLin)
    {
        _settings.setBusType(BusType::LIN);
        _settings.setLinBaudRate(19200);
    }
    else if (fd_support)
    {
        _settings.setFdBitrate(2000000);
        _settings.setFdSamplePoint(750);
    }

    _status.can_state.store(state_bus_off);
    _status.rx_count.store(0);
    _status.rx_errors.store(0);
    _status.rx_overruns.store(0);
    _status.tx_count.store(0);
    _status.tx_errors.store(0);
    _status.tx_dropped.store(0);

    _lastStateMsec = QDateTime::currentMSecsSinceEpoch();
    _lastReadMsec = QDateTime::currentMSecsSinceEpoch();

    if (m_GrIPHandler)
        refreshCapabilities();
}

GrIPInterface::~GrIPInterface()
{
}

QString GrIPInterface::getDetailsStr() const
{
    if (_manufacturer == CANIL_LIN)
    {
        return tr("CANIL with LIN support");
    }
    if (_manufacturer == CANIL_CAN)
    {
        return (_deviceCaps & GRIP_CAP_CAN_FD)
                   ? tr("CANIL with CANFD support")
                   : tr("CANIL with standard CAN support");
    }
    return tr("Not Supported");
}

QString GrIPInterface::getName() const
{
    return _name;
}

void GrIPInterface::setName(QString name)
{
    _name = name;
}

QList<CanTiming> GrIPInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;

    if (_manufacturer != GrIPInterface::CANIL_CAN)
        return retval;

    struct RateEntry { uint32_t rate; uint32_t capBit; };
    static constexpr RateEntry kCanRates[] = {
        {   10000, GRIP_CAP_CAN_BAUD_10K  },
        {   20000, GRIP_CAP_CAN_BAUD_20K  },
        {   50000, GRIP_CAP_CAN_BAUD_50K  },
        {  100000, GRIP_CAP_CAN_BAUD_100K },
        {  125000, GRIP_CAP_CAN_BAUD_125K },
        {  250000, GRIP_CAP_CAN_BAUD_250K },
        {  500000, GRIP_CAP_CAN_BAUD_500K },
        {  800000, 0                       },  // no capability bit — include only in fallback
        { 1000000, GRIP_CAP_CAN_BAUD_1M   },
    };
    static constexpr unsigned kFdRates[]       = {2000000, 5000000};
    static constexpr unsigned kFdSamplePoints[] = {750, 750};

    const bool filterByDevice = (_deviceCaps != 0);
    const bool fd = supportsCanFD();

    unsigned i = 0;
    for (auto &e : kCanRates)
    {
        if (filterByDevice && e.capBit != 0 && !(_deviceCaps & e.capBit))
            continue;
        if (filterByDevice && e.capBit == 0)
            continue;  // 800K has no firmware bit — skip when device caps are known

        if (fd)
        {
            for (size_t f = 0; f < std::size(kFdRates); ++f)
                retval << CanTiming(i++, e.rate, kFdRates[f], 875, kFdSamplePoints[f]);
        }
        else
        {
            retval << CanTiming(i++, e.rate, 0, 875, 0);
        }
    }

    return retval;
}

void GrIPInterface::applyConfig(const MeasurementInterface &mi)
{
    _settings = mi;
}

bool GrIPInterface::updateStatus()
{
    if (_manufacturer == CANIL_CAN)
    {
        _status.can_state.store(m_GrIPHandler->CanGetState(_channel_idx));
        _status.rx_overruns.store(m_GrIPHandler->CanGetRxDropCount(_channel_idx));
    }
    else if (_manufacturer == CANIL_LIN)
    {
        _status.can_state.store(m_GrIPHandler->LinGetState(_channel_idx));
    }
    return true;
}

bool GrIPInterface::readConfig()
{
    return false;
}

bool GrIPInterface::readConfigFromLink(rtnl_link *link)
{
    Q_UNUSED(link);
    return false;
}

bool GrIPInterface::supportsTimingConfiguration()
{
    return _config.supports_timing;
}

bool GrIPInterface::supportsCanFD()
{
    return (_deviceCaps != 0) ? ((_deviceCaps & GRIP_CAP_CAN_FD) != 0) : _config.supports_canfd;
}

bool GrIPInterface::supportsTripleSampling()
{
    return false;
}

unsigned GrIPInterface::getBitrate()
{
    return _settings.bitrate();
}

BusType GrIPInterface::busType() const
{
    return _isLin ? BusType::LIN : BusType::CAN;
}

uint32_t GrIPInterface::getCapabilities()
{
    if (_deviceCaps == 0)
    {
        // Not yet queried — return a safe default so the UI is not completely blank.
        if (_isLin)
            return 0;
        uint32_t retval = BusInterface::capability_auto_restart | BusInterface::capability_listen_only;
        if (_config.supports_canfd)
            retval |= BusInterface::capability_canfd;
        return retval;
    }

    if (_isLin)
    {
        uint32_t retval = 0;
        if (_deviceCaps & GRIP_CAP_LIN_MODE_MASTER) retval |= BusInterface::capability_lin_master;
        if (_deviceCaps & GRIP_CAP_LIN_MODE_SLAVE)  retval |= BusInterface::capability_lin_slave;
        return retval;
    }

    uint32_t retval = 0;
    if (_deviceCaps & GRIP_CAP_CAN_LISTEN_ONLY) retval |= BusInterface::capability_listen_only;
    if (_deviceCaps & GRIP_CAP_CAN_ABOM)        retval |= BusInterface::capability_auto_restart;
    if (_deviceCaps & GRIP_CAP_CAN_FD)          retval |= BusInterface::capability_canfd;
    return retval;
}

void GrIPInterface::refreshCapabilities()
{
    const uint8_t busType = _isLin ? 1u : 0u;
    m_GrIPHandler->RequestChannelCapabilities(busType, static_cast<uint8_t>(_channel_idx));
    QThread::msleep(50);
    _deviceCaps = m_GrIPHandler->GetChannelCapabilities(busType, static_cast<uint8_t>(_channel_idx));
}

bool GrIPInterface::updateStatistics()
{
    return updateStatus();
}

void GrIPInterface::resetStatistics()
{
    _status.rx_count.store(0);
    _status.rx_errors.store(0);
    _status.rx_overruns.store(0);
    _status.tx_count.store(0);
    _status.tx_errors.store(0);
    _status.tx_dropped.store(0);

    BusInterface::resetStatistics();
}

uint32_t GrIPInterface::getState()
{
    switch (_status.can_state.load())
    {
    case CANIL_CAN_State::CAN_Active:
        return state_ok;
    case CANIL_CAN_State::CAN_ErrorWarning:
        return state_warning;
    case CANIL_CAN_State::CAN_ErrorPassiv:
        return state_passive;
    case CANIL_CAN_State::CAN_Off:
        return state_bus_off;
    case CANIL_CAN_State::CAN_Stopped:
        return state_stopped;
    default:
        return state_unknown;
    }
   return state_unknown;
}

int GrIPInterface::getNumRxFrames()
{
    return static_cast<int>(_status.rx_count.load());
}

int GrIPInterface::getNumRxErrors()
{
    return _status.rx_errors.load();
}

int GrIPInterface::getNumTxFrames()
{
    return static_cast<int>(_status.tx_count.load());
}

int GrIPInterface::getNumTxErrors()
{
    return _status.tx_errors.load();
}

int GrIPInterface::getNumRxOverruns()
{
    return static_cast<int>(_status.rx_overruns.load());
}

int GrIPInterface::getNumTxDropped()
{
    return static_cast<int>(_status.tx_dropped.load());
}

int GrIPInterface::getIfIndex()
{
    return _idx;
}

QString GrIPInterface::getVersion()
{
    if (!_version.isEmpty())
        return _version;
    // Requested by the driver at discovery; open() copies it into _version.
    const QString version = m_GrIPHandler ? QString::fromStdString(m_GrIPHandler->GetVersion()) : QString();
    return version.isEmpty() ? BusInterface::getVersion() : version;
}

void GrIPInterface::open()
{
    if (m_GrIPHandler == nullptr)
    {
        _isOpen = false;
        _isOffline = true;
        return;
    }

    refreshCapabilities();

    // Poll for the firmware version string populated by GrIPHandler after
    // connecting. Typically available within one or two iterations.
    for (int i = 0; i < 15; i++)
    {
        _version = QString::fromStdString(m_GrIPHandler->GetVersion());
        if (!_version.isEmpty())
        {
            break;
        }
        QThread::msleep(2);
    }

    if (_manufacturer == CANIL_CAN)
    {
        // Disable the channel before reconfiguring to avoid spurious traffic.
        m_GrIPHandler->CanEnableChannel(_channel_idx, false);
        QThread::msleep(2);

        // Apply bit rate — use custom value if set, otherwise use the selected preset.
        const uint32_t baud = _settings.isCustomBitrate() ? _settings.customBitrate() : _settings.bitrate();
        const uint32_t arbBaud = baud > 0 ? baud : 500000;

        const bool channelSupportsFd = (_deviceCaps & GRIP_CAP_CAN_FD) != 0;

        if (channelSupportsFd)
        {
            // Send CAN FD config with separate data-phase baudrate.
            const uint32_t dataBaud = _settings.isCustomFdBitrate()
                ? _settings.customFdBitrate()
                : _settings.fdBitrate();
            m_GrIPHandler->CanSetFdConfig(_channel_idx, arbBaud, dataBaud > 0 ? dataBaud : arbBaud,
                                          _settings.isListenOnlyMode(), true, _settings.doAutoRestart());
        }
        else
        {
            m_GrIPHandler->CanSetConfig(_channel_idx, arbBaud, _settings.isListenOnlyMode(), true, _settings.doAutoRestart());
        }

        QThread::msleep(20);
        m_GrIPHandler->CanEnableChannel(_channel_idx, true);
    }
    else if (_manufacturer == CANIL_LIN)
    {
        // Disable the channel before reconfiguring to avoid spurious traffic.
        m_GrIPHandler->LinEnableChannel(_channel_idx, false);
        QThread::msleep(2);

        // LIN only enabled if valid LDF is loaded
        if (!_settings.linLdfPath().isEmpty())
        {
            LinDb ldb;
            if (ldb.loadFile(_settings.linLdfPath()))
            {
                const bool isMaster     = _settings.linNodeMode() == LinNodeMode::Master;
                const bool isListenOnly = _settings.linListenOnly();
                const QString slaveNode = _settings.linSlaveNode();

                // mode: 0 = master, 1 = slave, 2 = slave_listenonly
                const uint8_t linMode = isMaster ? 0u : (isListenOnly ? 2u : 1u);

                // For master: use the first slave's diag timings; for slave: use own node's timings.
                const QString diagNode  = isMaster ? ldb.slaveNodes().value(0) : slaveNode;
                const LinDiagTiming timing = ldb.diagTiming(diagNode);

                const uint8_t slaveNad = static_cast<uint8_t>(ldb.nodeNad(diagNode));

                m_GrIPHandler->LinSetConfig(
                    _channel_idx,
                    _settings.linBaudRate(),
                    linMode,
                    static_cast<uint8_t>(_settings.linProtocolVersion()),
                    _settings.linTimebaseMs(),
                    _settings.linJitterUs(),
                    timing.stMinMs,
                    timing.p2MinMs,
                    timing.nAsMs,
                    timing.nCrMs,
                    slaveNad
                );
                QThread::msleep(5);

                if (!isListenOnly)
                {
                    const int ldfTableCount  = ldb.scheduleTableNames().size();
                    const int maxTableCount  = m_GrIPHandler->Channels_LinScheduleTables();
                    const int tableCount     = (maxTableCount > 0 && ldfTableCount > maxTableCount)
                                               ? maxTableCount : ldfTableCount;

                    if (maxTableCount > 0 && ldfTableCount > maxTableCount)
                        log_warning(QString("GrIP: LDF contains %1 schedule tables but device supports only %2 — truncating to %2.")
                                    .arg(ldfTableCount).arg(maxTableCount));

                    for (int tableIndex = 0; tableIndex < tableCount; ++tableIndex)
                    {
                        m_GrIPHandler->LinSetScheduleTable(_channel_idx, static_cast<uint8_t>(tableIndex));
                        QThread::msleep(2);

                        const auto entries = ldb.scheduleTableEntries(tableIndex);
                        for (const LinScheduleEntry &entry : entries)
                        {
                            if (!isMaster && entry.publisherName != slaveNode)
                                continue;

                            // TX when this node is the publisher of the frame, RX otherwise.
                            const bool isRX = isMaster ? entry.isMasterPublisher : (entry.publisherName == slaveNode);

                            BusMessage msg;
                            msg.setId(entry.frameId);
                            msg.setLength(entry.dlc);
                            msg.setRX(isRX);

                            const auto &defaults = _settings.linFrameDefaults();
                            if (auto it = defaults.find(static_cast<uint8_t>(entry.frameId)); it != defaults.end())
                            {
                                const QByteArray &payload = it.value();
                                for (int i = 0; i < payload.size() && i < entry.dlc; ++i)
                                    msg.setDataAt(static_cast<uint8_t>(i), static_cast<uint8_t>(payload[i]));
                            }

                            m_GrIPHandler->LinAddFrame(_channel_idx, msg, entry.delayMs, entry.isSporadic);
                        }
                    }

                    // Activate the user-selected schedule table last
                    m_GrIPHandler->LinSetScheduleTable(_channel_idx, _settings.linScheduleTableIndex());
                    QThread::msleep(2);
                }
            }
            QThread::msleep(10);

            m_GrIPHandler->LinEnableChannel(_channel_idx, true);
        }
    }

    _isOpen = true;
    _isOffline = false;
    _status.rx_count.store(0);
    _status.rx_errors.store(0);
    _status.rx_overruns.store(0);
    _status.tx_count.store(0);
    _status.tx_errors.store(0);
    _status.tx_dropped.store(0);
}

void GrIPInterface::handleSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::ResourceError)
    {
        _isOffline = true;
    }

    static const auto toErrorString = [](QSerialPort::SerialPortError err) -> QString
    {
        switch (err)
        {
        case QSerialPort::NoError:
            return {};
        case QSerialPort::DeviceNotFoundError:
            return QStringLiteral("Device not found");
        case QSerialPort::PermissionError:
            return QStringLiteral("Permission denied");
        case QSerialPort::OpenError:
            return QStringLiteral("Open error");
        case QSerialPort::WriteError:
            return QStringLiteral("Write error");
        case QSerialPort::ReadError:
            return QStringLiteral("Read error");
        case QSerialPort::ResourceError:
            return QStringLiteral("Resource error");
        case QSerialPort::UnsupportedOperationError:
            return QStringLiteral("Unsupported operation");
        case QSerialPort::TimeoutError:
            return {};
        case QSerialPort::NotOpenError:
            return QStringLiteral("Not open error");
        default:
            return QStringLiteral("Unknown error");
        }
    };

    const QString msg = toErrorString(error);
    if (!msg.isEmpty())
    {
        qWarning() << "Serial port error:" << msg;
    }
}

void GrIPInterface::close()
{
    _isOpen = false;

    if (_manufacturer == CANIL_CAN)
    {
        m_GrIPHandler->CanEnableChannel(_channel_idx, false);
    }
    else if (_manufacturer == CANIL_LIN)
    {
        m_GrIPHandler->LinEnableChannel(_channel_idx, false);
    }
}

bool GrIPInterface::isOpen()
{
    return _isOpen;
}

void GrIPInterface::sendLinSleepWakeup(bool wakeup)
{
    if (_isLin && _isOpen)
        m_GrIPHandler->LinSleepWakeup(_channel_idx, wakeup);
}

void GrIPInterface::setLinScheduleTable(uint8_t tableIndex)
{
    if (_isLin && _isOpen)
        m_GrIPHandler->LinSetScheduleTable(_channel_idx, tableIndex);
}

void GrIPInterface::sendLinDiagRequest(uint8_t nad, const uint8_t *data, uint8_t len)
{
    if (_isLin && _isOpen)
        m_GrIPHandler->LinSendDiagRequest(_channel_idx, nad, data, len);
}

void GrIPInterface::sendMessage(const BusMessage &msg)
{
    if (_manufacturer == CANIL_CAN)
    {
        if (!m_GrIPHandler->CanTransmit(_channel_idx, msg))
        {
            _status.tx_errors.fetch_add(1);
        }
    }
    else if (_manufacturer == CANIL_LIN)
    {
        if (!m_GrIPHandler->LinSendData(_channel_idx, msg))
        {
            _status.tx_errors.fetch_add(1);
        }
    }
}

bool GrIPInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    Q_UNUSED(timeout_ms);

    if (_manufacturer == CANIL_CAN)
    {
        while (m_GrIPHandler->CanAvailable(_channel_idx))
        {
            auto msg = m_GrIPHandler->CanReceive(_channel_idx);
            msg.setInterfaceId(getId());

            if (!msg.isRX())
            {
                // TX echo frame
                if (!msg.isErrorFrame())
                {
                    _status.tx_count.fetch_add(1);
                    addFrameBits(msg);
                }
                else
                {
                    _status.tx_errors.fetch_add(1);
                }
                msglist.append(msg);
            }
            else
            {
                msglist.append(msg);
                _status.rx_count.fetch_add(1);
                addFrameBits(msg);
            }
        }
    }
    else if (_manufacturer == CANIL_LIN)
    {
        while (m_GrIPHandler->LinAvailable(_channel_idx))
        {
            auto msg = m_GrIPHandler->LinReceive(_channel_idx);
            msg.setInterfaceId(getId());

            if (msg.isRX())
            {
                _status.rx_count.fetch_add(1);
            }
            else
            {
                _status.tx_count.fetch_add(1);
            }
            msglist.append(msg);
        }
    }

    QThread::msleep(1);

    if (_isOffline)
    {
        if (_isOpen)
        {
            close();
        }
        return false;
    }

    return true;
}
