#include "LindeApiInterface.h"
#include "LindeApiDriver.h"

#include "core/Log.h"
#include "core/MeasurementInterface.h"
#include "core/DBC/LinDb.h"

#include <QMutexLocker>
#include <QDateTime>
#include <QThread>

#include <algorithm>
#include <cstring>


LindeApiInterface::LindeApiInterface(LindeApiDriver *driver,
                                     std::shared_ptr<LindeSharedDevice> sharedDev,
                                     uint8_t channel)
    : BusInterface(driver),
      _channel(channel),
      _sharedDev(std::move(sharedDev))
{
    _settings.setBusType(BusType::LIN);
    _settings.setLinBaudRate(19200);
}

LindeApiInterface::~LindeApiInterface()
{
    if (_isOpen.load())
        close();
}

QString LindeApiInterface::getName() const
{
    return _sharedDev->productName
           + QString::number(_sharedDev->deviceIndex)
           + "_CH"
           + QString::number(_channel);
}

QString LindeApiInterface::getDetailsStr() const
{
    return QStringLiteral("LIN Interface CH%1").arg(_channel);
}

void LindeApiInterface::applyConfig(const MeasurementInterface &mi)
{
    _settings = mi;
}

unsigned LindeApiInterface::getBitrate()
{
    return _settings.linBaudRate();
}

uint32_t LindeApiInterface::getCapabilities()
{
    return capability_lin_master | capability_lin_slave;
}

QList<CanTiming> LindeApiInterface::getAvailableBitrates()
{
    // Standard LIN baud rates as id/bitrate pairs (no FD, sample point unused for LIN).
    static const unsigned rates[] = {1200, 2400, 4800, 9600, 10417, 19200, 20000};
    QList<CanTiming> result;
    unsigned id = 0;
    for (unsigned r : rates)
        result.append(CanTiming(id++, r, 0, 0));
    return result;
}

void LindeApiInterface::open()
{
    QMutexLocker lock(&_sharedDev->openMutex);

    if (_isOpen.load())
        return;

    if (_sharedDev->openCount == 0)
    {
        if (!_sharedDev->open())
        {
            log_error(QStringLiteral("LindeAPI: open failed: %1")
                          .arg(QString::fromStdString(_sharedDev->getLastError())));
            return;
        }

        _sharedDev->startReader();
        _sharedDev->resetTimestampEpoch();
    }

    _sharedDev->openCount++;

    if (_channel >= _sharedDev->channelCount)
    {
        log_error(QStringLiteral("LindeAPI: channel %1 not available (device has %2)")
                      .arg(_channel).arg(_sharedDev->channelCount));
        releaseSharedDevice();
        return;
    }

    // Drop anything left from an earlier session; this channel's frames are queued from here on.
    _sharedDev->setChannelOpen(_channel, true);

    // Per-channel configuration. openMutex is held for the whole sequence so a
    // second channel's BusListener thread cannot interleave its EP0 transfers
    // (setBusConfig/setMode/schedule upload) with this channel's sequence.
    _sharedDev->setHostFormat(_channel);

    const bool isMaster     = _settings.linNodeMode() == LinNodeMode::Master;
    const bool isListenOnly = _settings.linListenOnly() ||
                              _settings.linNodeMode() == LinNodeMode::Monitor;

    // Map LinProtocolVersion enum → LIN_USB_VERSION_* constant
    static const uint8_t protocolMap[] = {
        LIN_USB_VERSION_1_3,
        LIN_USB_VERSION_2_0,
        LIN_USB_VERSION_2_1,
        LIN_USB_VERSION_2_2,
        LIN_USB_VERSION_2_2A,
    };
    const auto protVer = static_cast<int>(_settings.linProtocolVersion());
    const uint8_t linVersion = (protVer >= 0 && protVer < 5) ? protocolMap[protVer]
                                                              : LIN_USB_VERSION_2_1;

    lin_usb_bus_config_t busCfg{};
    busCfg.baudrate     = _settings.linBaudRate();
    busCfg.lin_version  = linVersion;
    busCfg.break_length = 13; // standard LIN break length
    busCfg.timebase_ms  = _settings.linTimebaseMs();
    busCfg.jitter_us    = _settings.linJitterUs();
    busCfg.flags        = (isMaster && !isListenOnly) ? LIN_USB_FLAG_MASTER : 0u;

    // Load LDF diagnostic timings if available
    const QString ldfPath = _settings.linLdfPath();
    if (!ldfPath.isEmpty())
    {
        LinDb ldb;
        if (ldb.loadFile(ldfPath))
        {
            const QString diagNode = isMaster
                                     ? ldb.slaveNodes().value(0)
                                     : _settings.linSlaveNode();
            const LinDiagTiming timing = ldb.diagTiming(diagNode);
            busCfg.slave_nad     = ldb.nodeNad(diagNode);
            busCfg.diag_stmin_ms = timing.stMinMs;
            busCfg.diag_p2min_ms = timing.p2MinMs;
            busCfg.diag_nas_ms   = timing.nAsMs;
            busCfg.diag_ncr_ms   = timing.nCrMs;
        }
    }

    // Without a bus config or a clean STOP the channel would look open while doing nothing.
    if (!_sharedDev->setBusConfig(_channel, busCfg))
    {
        log_error(QStringLiteral("LindeAPI: setBusConfig failed: %1")
                      .arg(QString::fromStdString(_sharedDev->getLastError())));
        releaseSharedDevice();
        return;
    }

    if (!_sharedDev->setMode(_channel, LIN_USB_MODE_STOP))
    {
        log_error(QStringLiteral("LindeAPI: setMode failed: %1")
                      .arg(QString::fromStdString(_sharedDev->getLastError())));
        releaseSharedDevice();
        return;
    }

    // Upload schedule tables if master mode and LDF is provided
    std::memset(_tableEntryCounts, 0, sizeof(_tableEntryCounts));
    _activeTable = 0;
    if (isMaster && !isListenOnly && !ldfPath.isEmpty())
    {
        LinDb ldb;
        if (ldb.loadFile(ldfPath))
        {
            // Firmware rejects tables/slots beyond its limits, so never upload them.
            const int deviceTables = (_sharedDev->scheduleTables > 0) ? _sharedDev->scheduleTables
                                                                      : MAX_TABLES;
            const int ldfTables  = static_cast<int>(ldb.scheduleTableNames().size());
            const int tableCount = std::min({ldfTables, deviceTables, static_cast<int>(MAX_TABLES)});
            if (ldfTables > tableCount)
            {
                log_warning(QStringLiteral("LindeAPI: LDF has %1 schedule tables, device supports %2; extra tables ignored")
                                .arg(ldfTables).arg(tableCount));
            }

            for (int t = 0; t < tableCount; t++)
            {
                const auto entries = ldb.scheduleTableEntries(t);
                const int  entryCount = std::min(static_cast<int>(entries.size()),
                                                 static_cast<int>(LIN_USB_MAX_SCHEDULE_ENTRIES));
                if (entries.size() > entryCount)
                {
                    log_warning(QStringLiteral("LindeAPI: schedule table %1 has %2 entries, device supports %3; extra entries ignored")
                                    .arg(t).arg(entries.size()).arg(LIN_USB_MAX_SCHEDULE_ENTRIES));
                }

                for (int s = 0; s < entryCount; s++)
                {
                    const LinScheduleEntry &le = entries[s];

                    lin_usb_schedule_entry_t se{};
                    se.lin_id    = le.frameId;
                    se.dlc       = le.dlc;
                    se.period_ms = le.delayMs;
                    se.table_id  = static_cast<uint8_t>(t);
                    se.flags     = le.isSporadic ? LIN_USB_FRAME_FLAG_SPORADIC : 0u;

                    // Direction: 0 = device is publisher (TX), 1 = subscriber (RX).
                    // In master mode, master publishes when isMasterPublisher is set.
                    se.direction = le.isMasterPublisher ? 0u : 1u;

                    if (le.isMasterPublisher)
                    {
                        // Fill default payload if available
                        const auto &defaults = _settings.linFrameDefaults();
                        if (auto it = defaults.find(le.frameId); it != defaults.end())
                        {
                            const QByteArray &payload = it.value();
                            for (int i = 0; i < payload.size() && i < le.dlc; i++)
                                se.data[i] = static_cast<uint8_t>(payload[i]);
                        }
                    }

                    if (!_sharedDev->uploadScheduleEntry(_channel,
                                                         static_cast<uint8_t>(t),
                                                         static_cast<uint8_t>(s), se))
                    {
                        log_warning(QStringLiteral("LindeAPI: uploadScheduleEntry failed for table %1 slot %2")
                                        .arg(t).arg(s));
                    }
                }

                _tableEntryCounts[t] = static_cast<uint8_t>(entryCount);
            }

            if (tableCount > 0)
            {
                // Activate the user-selected table
                _activeTable = static_cast<uint8_t>(std::clamp(static_cast<int>(_settings.linScheduleTableIndex()),
                                                               0, tableCount - 1));
                if (!_sharedDev->scheduleStart(_channel, _activeTable, _tableEntryCounts[_activeTable]))
                {
                    log_warning(QStringLiteral("LindeAPI: scheduleStart failed: %1")
                                    .arg(QString::fromStdString(_sharedDev->getLastError())));
                }
            }
            else
            {
                log_warning(QStringLiteral("LindeAPI: LDF has no schedule tables, master schedule not started"));
            }
        }
    }
    else if (!isMaster && !isListenOnly)
    {
        // Slave mode. The firmware slave answers a PID only if the frame is in
        // the table activated by MODE_START, so every frame this node publishes
        // goes into table 0, one slot each, deduplicated by ID across all LDF
        // tables. The master's schedule timing is irrelevant here.
        static constexpr uint8_t SLAVE_TABLE     = 0;
        static constexpr unsigned MAX_SLAVE_SLOTS = LIN_USB_MAX_SCHEDULE_ENTRIES;

        uint8_t slot = 0;

        if (!ldfPath.isEmpty())
        {
            LinDb ldb;
            if (ldb.loadFile(ldfPath))
            {
                const QString slaveNode = _settings.linSlaveNode();
                const int tableCount = ldb.scheduleTableNames().size();
                QSet<uint8_t> uploaded;

                for (int t = 0; t < tableCount; t++)
                {
                    for (const LinScheduleEntry &le : ldb.scheduleTableEntries(t))
                    {
                        if (le.publisherName != slaveNode)
                            continue;
                        if (uploaded.contains(le.frameId))
                            continue;
                        if (slot >= MAX_SLAVE_SLOTS)
                        {
                            log_warning(QStringLiteral("LindeAPI: slave publishes more than %1 frames, ID 0x%2 dropped")
                                            .arg(MAX_SLAVE_SLOTS).arg(le.frameId, 2, 16, QLatin1Char('0')));
                            continue;
                        }
                        uploaded.insert(le.frameId);

                        lin_usb_schedule_entry_t se{};
                        se.lin_id    = le.frameId;
                        se.dlc       = le.dlc;
                        se.table_id  = SLAVE_TABLE;
                        se.direction = 0u; // 0 = device is publisher: this slave sends the response

                        const auto &defaults = _settings.linFrameDefaults();
                        if (auto it = defaults.find(le.frameId); it != defaults.end())
                        {
                            const QByteArray &payload = it.value();
                            for (int i = 0; i < payload.size() && i < le.dlc; i++)
                                se.data[i] = static_cast<uint8_t>(payload[i]);
                        }

                        if (!_sharedDev->uploadScheduleEntry(_channel, SLAVE_TABLE, slot, se))
                        {
                            log_warning(QStringLiteral("LindeAPI: uploadScheduleEntry failed for slave slot %1")
                                            .arg(slot));
                        }
                        slot++;
                    }
                }
            }
        }

        // Start even with no frames: the slave then still listens and reports
        // diagnostic frames, instead of leaving the transceiver disabled.
        _activeTable = SLAVE_TABLE;
        _tableEntryCounts[SLAVE_TABLE] = slot;
        _sharedDev->scheduleStart(_channel, SLAVE_TABLE, slot);
    }

    _numRx = 0;
    _numTx = 0;
    _numTxErr = 0;
    _isOpen.store(true);
}

void LindeApiInterface::close()
{
    // openMutex spans the whole teardown: releasing it before stopReader()/close()
    // let another channel's open() start a second reader on a device being torn down.
    QMutexLocker lock(&_sharedDev->openMutex);
    if (!_isOpen.load())
        return;

    _isOpen.store(false);
    _sharedDev->setMode(_channel, LIN_USB_MODE_STOP);
    releaseSharedDevice();
}

void LindeApiInterface::releaseSharedDevice()
{
    _sharedDev->setChannelOpen(_channel, false);
    if (--_sharedDev->openCount == 0)
    {
        _sharedDev->stopReader();
        _sharedDev->close();
    }
}

bool LindeApiInterface::isOpen()
{
    return _isOpen.load();
}

void LindeApiInterface::sendMessage(const BusMessage &msg)
{
    lin_usb_host_frame_t frame{};
    frame.echo_id  = 0u;   // non-RX echo_id signals TX
    frame.channel  = _channel;
    frame.lin_id   = static_cast<uint8_t>(msg.getId());
    frame.dlc      = msg.getLength();

    for (uint8_t i = 0; i < frame.dlc && i < 8u; i++)
        frame.data[i] = msg.getByte(i);

    if (msg.isLinSleepFrame())
        frame.flags |= LIN_USB_FRAME_FLAG_SLEEP;

    if (!_sharedDev->sendFrame(frame))
    {
        _numTxErr++;
        log_error(QStringLiteral("LindeAPI: sendFrame failed: %1")
                      .arg(QString::fromStdString(_sharedDev->getLastError())));
        return;
    }
    _numTx++;
}

bool LindeApiInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    lin_usb_host_frame_t frame{};
    if (!_sharedDev->readFrame(_channel, frame, timeout_ms))
        return false;

    BusMessage msg;
    msg.setInterfaceId(getId());
    msg.setBusType(BusType::LIN);
    msg.setId(frame.lin_id);
    msg.setLength(frame.dlc);

    for (uint8_t i = 0; i < frame.dlc && i < 8u; i++)
        msg.setDataAt(i, frame.data[i]);

    msg.setRX(frame.echo_id == LIN_USB_ECHO_ID_RX);

    if (frame.flags & LIN_USB_FRAME_FLAG_ERROR)
    {
        msg.setErrorFrame(true);
        if (!(frame.flags & LIN_USB_FRAME_FLAG_RESPONDED))
            msg.setErrorFlag(BusError::LinNotResponded);
        if (!(frame.flags & LIN_USB_FRAME_FLAG_VALID))
            msg.setErrorFlag(BusError::LinChecksumError);
    }

    // Timestamp
    if (frame.timestamp_ms != 0)
        msg.setTimestamp_ms(_sharedDev->deviceTimestampToHostMs(frame.timestamp_ms));
    else
        msg.setTimestamp_ms(QDateTime::currentMSecsSinceEpoch());

    _numRx++;
    msglist.append(msg);
    return true;
}

void LindeApiInterface::sendLinSleepWakeup(bool wakeup)
{
    if (!_isOpen.load())
        return;
    if (wakeup)
        _sharedDev->wakeup(_channel);
    else
        _sharedDev->goToSleep(_channel);
}

void LindeApiInterface::setLinScheduleTable(uint8_t tableIndex)
{
    if (!_isOpen.load())
        return;
    if (tableIndex >= MAX_TABLES)
    {
        log_warning(QStringLiteral("LindeAPI: schedule table %1 out of range").arg(tableIndex));
        return;
    }
    _sharedDev->scheduleStop(_channel);
    _activeTable = tableIndex;
    const uint8_t count = _tableEntryCounts[tableIndex];
    if (count > 0)
    {
        _sharedDev->scheduleStart(_channel, tableIndex, count);
    }
}

void LindeApiInterface::sendLinDiagRequest(uint8_t nad, const uint8_t *data, uint8_t len)
{
    if (!_isOpen.load())
        return;

    lin_usb_host_frame_t frame{};
    frame.echo_id  = 0u;
    frame.channel  = _channel;
    frame.lin_id   = 0x3Cu; // MasterReq
    frame.dlc      = 8u;
    frame.data[0]  = nad;

    const uint8_t copyLen = std::min(len, static_cast<uint8_t>(7u));
    std::memcpy(&frame.data[1], data, copyLen);

    _sharedDev->sendFrame(frame);
}

bool LindeApiInterface::updateStatistics()
{
    return true;
}

uint32_t LindeApiInterface::getState()
{
    if (!_isOpen.load())
        return state_stopped;

    if (!(_sharedDev->features & LIN_USB_FEATURE_BUS_STATE))
        return state_ok;

    lin_usb_bus_state_t hwState{};
    if (!_sharedDev->getBusState(_channel, hwState))
        return state_unknown;

    switch (static_cast<lin_usb_bus_state_e>(hwState.state))
    {
    case LIN_USB_BUS_STATE_OK:      return state_ok;
    case LIN_USB_BUS_STATE_PASSIVE: return state_passive;
    case LIN_USB_BUS_STATE_BUS_OFF: return state_bus_off;
    case LIN_USB_BUS_STATE_ERROR:   return state_warning;
    default:                        return state_unknown;
    }
}

int LindeApiInterface::getNumRxFrames()  { return static_cast<int>(_numRx); }
int LindeApiInterface::getNumRxErrors()  { return 0; }
int LindeApiInterface::getNumTxFrames()  { return static_cast<int>(_numTx); }
int LindeApiInterface::getNumTxErrors()  { return static_cast<int>(_numTxErr); }
int LindeApiInterface::getNumRxOverruns(){ return static_cast<int>(_sharedDev->getRxOverruns(_channel)); }
int LindeApiInterface::getNumTxDropped() { return 0; }
