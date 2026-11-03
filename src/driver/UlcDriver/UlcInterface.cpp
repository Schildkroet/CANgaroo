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

#include "UlcInterface.h"
#include "UlcDriver.h"

#include <algorithm>
#include <chrono>

#include <QElapsedTimer>
#include <QMutexLocker>

#include "core/Log.h"
#include "db/model/LinDb.h"

#include "UlcFrameCodec.h"

static int64_t nowMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}


// ── Construction ─────────────────────────────────────────────────────────────

UlcInterface::UlcInterface(UlcDriver *driver, int index, QString portName)
    : BusInterface(driver)
    , _idx(index)
    , _name(std::move(portName))
{
    _settings.setBusType(BusType::LIN);
    _settings.setLinBaudRate(19200);
}

UlcInterface::~UlcInterface()
{
    if (_isOpen)
        close();
}


// ── Identity ─────────────────────────────────────────────────────────────────

QString UlcInterface::getName() const
{
    return _name;
}

void UlcInterface::setName(QString portName)
{
    _name = std::move(portName);
}

QString UlcInterface::getDetailsStr() const
{
    return tr("uCAN USB LIN Converter (master / slave / monitor)");
}

QString UlcInterface::getVersion()
{
    // Read from the device on open(); before that it is not queried.
    return _version.isEmpty() ? BusInterface::getVersion() : _version;
}

int UlcInterface::getIfIndex() const noexcept
{
    return _idx;
}


// ── Configuration ─────────────────────────────────────────────────────────────

QList<CanTiming> UlcInterface::getAvailableBitrates()
{
    // 9600 and 19200 work with every firmware, the others need firmware 3.x.
    static const unsigned rates[] = {2400, 4800, 9600, 10417, 19200, 20000};
    QList<CanTiming> result;
    unsigned id = 0;
    for (unsigned r : rates)
        result.append(CanTiming(id++, r, 0, 0));
    return result;
}

void UlcInterface::applyConfig(const MeasurementInterface &mi)
{
    _settings = mi;
}

unsigned UlcInterface::getBitrate()
{
    return _settings.linBaudRate();
}

uint32_t UlcInterface::getCapabilities()
{
    return capability_lin_master | capability_lin_slave | capability_lin_monitor;
}


// ── LDF → schedule rows ───────────────────────────────────────────────────────

bool UlcInterface::loadLdf(LinDb &ldb) const
{
    const QString path = _settings.linLdfPath();
    if (path.isEmpty())
        return false;

    if (!ldb.loadFile(path))
    {
        log_warning(tr("uLC: cannot load LDF %1: %2").arg(path, ldb.lastError()));
        return false;
    }
    return true;
}

QByteArray UlcInterface::defaultPayload(uint8_t id, uint8_t len) const
{
    // Data sent from cangaroo during the session wins over the LDF defaults,
    // so switching the schedule table keeps it.
    QByteArray source;
    {
        QMutexLocker lock(&_txMutex);
        source = _sentPayloads.value(id);
    }
    if (source.isNull())
        source = _settings.linFrameDefaults().value(id);

    QByteArray payload(len, '\0');
    for (int i = 0; i < source.size() && i < len; ++i)
        payload[i] = source[i];
    return payload;
}

QList<QByteArray> UlcInterface::masterTableCommands(const LinDb &ldb, int tableIndex,
                                                    QSet<uint8_t> &published) const
{
    struct Slot
    {
        LinScheduleEntry entry;
        unsigned delayMs;
    };

    QList<Slot> slotList;
    QSet<uint8_t> seen;
    int repeated = 0;
    int dropped = 0;

    for (const LinScheduleEntry &le : ldb.scheduleTableEntries(tableIndex))
    {
        // The firmware keys rows by frame ID, so a frame can only have one slot,
        // and it has a fixed number of slots. A slot that cannot be uploaded
        // gives its time to the slot before it: the cycle keeps the LDF's length,
        // only the skipped frame goes out less often.
        const bool isRepeat = seen.contains(le.frameId);
        if (isRepeat || slotList.size() >= ulc::k_maxScheduleSlots)
        {
            (isRepeat ? repeated : dropped)++;
            if (!slotList.isEmpty())
                slotList.last().delayMs += le.delayMs;
            continue;
        }
        seen.insert(le.frameId);
        slotList.append(Slot{le, le.delayMs});
    }

    if (repeated > 0)
    {
        log_warning(tr("uLC: schedule table %1 repeats %2 frame(s); the device keeps one slot per frame ID, "
                       "so those frames go out less often than in the LDF (cycle time is kept)")
                        .arg(tableIndex).arg(repeated));
    }
    if (dropped > 0)
    {
        log_warning(tr("uLC: schedule table %1 has more frames than the device's %2 slots, %3 frame(s) not scheduled")
                        .arg(tableIndex).arg(ulc::k_maxScheduleSlots).arg(dropped));
    }

    // The firmware waits a row's delay *before* sending it, counted from the
    // previous slot, while an LDF delay is the time *after* a slot until the
    // next one. So each row gets the delay of the slot before it (the table
    // wraps). Firmware before 3.4 starts that wait only once the previous frame
    // has ended, so there the previous frame's own time is taken off.
    const unsigned baud = _settings.linBaudRate();
    const bool waitFromFrameEnd = _fwVersionCode < ulc::k_fwSlotDelayFromFrameStart;

    QList<QByteArray> cmds;
    for (int i = 0; i < slotList.size(); ++i)
    {
        const LinScheduleEntry &le = slotList[i].entry;
        const Slot &prev = slotList[(i + slotList.size() - 1) % slotList.size()];

        unsigned waitMs = prev.delayMs;
        if (waitFromFrameEnd)
            waitMs -= std::min(waitMs, ulc::frameTimeMs(prev.entry.dlc, baud));

        const QByteArray payload = defaultPayload(le.frameId, le.dlc);
        // One byte on the wire; a merged delay beyond that is clamped.
        const uint8_t delayMs = static_cast<uint8_t>(std::clamp(waitMs, 1u, 255u));
        const uint8_t timeoutMs = ulc::responseTimeoutMs(le.dlc, baud);

        const QByteArray cmd = ulc::encodeScheduleSlot(
            le.frameId, le.dlc, le.isMasterPublisher,
            reinterpret_cast<const uint8_t *>(payload.constData()), delayMs, timeoutMs);
        if (cmd.isEmpty())
            continue;

        cmds.append(cmd);
        if (le.isMasterPublisher)
            published.insert(le.frameId);
    }
    return cmds;
}

QList<QByteArray> UlcInterface::slaveTableCommands(const LinDb &ldb, QSet<uint8_t> &published) const
{
    // The firmware slave only reacts to IDs in its table: frames this node
    // publishes go in as publisher rows (it answers them), the master's frames
    // as subscriber rows so the slave reports them instead of ignoring the ID.
    // Frames of other slaves are left out: if such a slave is not on the bus,
    // the firmware keeps waiting for its response and misses the next header.
    // Pass 0 adds the published frames, so they keep priority if slots run out.
    QList<QByteArray> cmds;
    QSet<uint8_t> seen;
    int dropped = 0;
    const QString slaveNode = _settings.linSlaveNode();
    const int tableCount = static_cast<int>(ldb.scheduleTableNames().size());

    for (int pass = 0; pass < 2; pass++)
    {
        const bool publisherPass = (pass == 0);

        for (int t = 0; t < tableCount; t++)
        {
            for (const LinScheduleEntry &le : ldb.scheduleTableEntries(t))
            {
                const bool ownFrame = le.publisherName == slaveNode;
                if (ownFrame != publisherPass)
                    continue;
                if (!ownFrame && !le.isMasterPublisher)
                    continue;
                if (seen.contains(le.frameId))
                    continue;
                if (cmds.size() >= ulc::k_maxScheduleSlots)
                {
                    dropped++;
                    continue;
                }
                seen.insert(le.frameId);

                QByteArray cmd;
                if (publisherPass)
                {
                    const QByteArray payload = defaultPayload(le.frameId, le.dlc);
                    cmd = ulc::encodeFrame(le.frameId,
                                           reinterpret_cast<const uint8_t *>(payload.constData()),
                                           le.dlc);
                    published.insert(le.frameId);
                }
                else
                {
                    cmd = ulc::encodeHeader(le.frameId, le.dlc);
                }

                if (!cmd.isEmpty())
                    cmds.append(cmd);
            }
        }
    }

    if (dropped > 0)
    {
        log_warning(tr("uLC: LDF has more frames than the device's %1 slave slots, %2 frame(s) not observable")
                        .arg(ulc::k_maxScheduleSlots).arg(dropped));
    }
    return cmds;
}


// ── Open / Close ──────────────────────────────────────────────────────────────

std::optional<QByteArray> UlcInterface::transact(const QByteArray &cmd, int timeout_ms)
{
    _port->write(cmd);
    _port->waitForBytesWritten(50);

    QElapsedTimer timer;
    timer.start();

    for (;;)
    {
        for (;;)
        {
            const int bel = _rxLineBuffer.indexOf('\x07');
            const int cr  = _rxLineBuffer.indexOf('\r');
            if (bel < 0 && cr < 0)
                break;

            if (bel >= 0 && (cr < 0 || bel < cr))
            {
                _rxLineBuffer.remove(0, bel + 1);
                return QByteArray("\x07");
            }

            const QByteArray line = _rxLineBuffer.left(cr);
            _rxLineBuffer.remove(0, cr + 1);

            // Bus traffic of a session still running when the port was opened;
            // no reply to a command starts with 't'.
            if (line.startsWith('t'))
                continue;
            return line;
        }

        // Data may already sit in QSerialPort's buffer: waitForBytesWritten()
        // also reads, and waitForReadyRead() would only wait for newer bytes.
        const qint64 remaining = timeout_ms - timer.elapsed();
        if (_port->bytesAvailable() == 0 &&
            (remaining <= 0 || !_port->waitForReadyRead(static_cast<int>(remaining))))
            return std::nullopt;
        _rxLineBuffer += _port->readAll();
    }
}

void UlcInterface::open()
{
    // A new session starts from the configured LDF defaults.
    {
        QMutexLocker lock(&_txMutex);
        _sentPayloads.clear();
    }

    _port = std::make_unique<QSerialPort>();
    _port->setPortName(_name);
    _port->setBaudRate(115200); // USB CDC: the line rate is ignored
    _port->setDataBits(QSerialPort::Data8);
    _port->setParity(QSerialPort::NoParity);
    _port->setStopBits(QSerialPort::OneStop);
    _port->setFlowControl(QSerialPort::NoFlowControl);

    auto fail = [this]() {
        _port->close();
        _port.reset();
        _isOpen = false;
        _isOffline = true;
    };

    if (!_port->open(QIODevice::ReadWrite))
    {
        log_error(tr("uLC: failed to open %1: %2").arg(_name, _port->errorString()));
        _port.reset();
        _isOpen = false;
        _isOffline = true;
        return;
    }
    _port->clear();
    _rxLineBuffer.clear();

    // Stop whatever an earlier session left running, then make sure a uLC is
    // answering: firmware before 3.5 uses ST's generic virtual COM port VID/PID.
    (void)transact(ulc::k_cmdClose);
    const QByteArray fwReply = transact(ulc::k_cmdFwVersion).value_or(QByteArray());
    const QByteArray fw = ulc::parseFirmwareVersion(fwReply);
    _fwVersionCode = ulc::parseFirmwareVersionCode(fwReply);
    if (fw.isEmpty())
    {
        log_error(tr("uLC: no uLC firmware answered on %1").arg(_name));
        fail();
        return;
    }
    _version = QString::fromLatin1(fw);
    if (const auto hw = transact(ulc::k_cmdHwVersion); hw && hw->startsWith('V'))
        _version += tr(" (HW %1)").arg(QString::fromLatin1(hw->mid(1)));

    // Baud rate before any slot: the firmware derives default timeouts from it.
    unsigned baud = _settings.linBaudRate();
    QByteArray baudCmd = ulc::encodeBaudRate(baud);
    const auto baudReply = baudCmd.isEmpty() ? std::nullopt : transact(baudCmd);
    if (!baudReply || *baudReply == "\x07")
    {
        log_warning(tr("uLC: baud rate %1 not supported by firmware %2, using 19200").arg(baud).arg(_version));
        baud = 19200;
        (void)transact(ulc::encodeBaudRate(baud));
    }

    const bool classic = _settings.linChecksumClassic() ||
                         _settings.linProtocolVersion() == LinProtocolVersion::V1_3;
    if (transact(ulc::encodeChecksum(classic)) != QByteArray("Z") && classic)
    {
        log_warning(tr("uLC: firmware %1 cannot switch to the classic checksum, update the firmware").arg(_version));
    }

    if (!ulc::isFrameAck(transact(ulc::k_cmdClearSchedule).value_or(QByteArray())))
        log_warning(tr("uLC: clearing the schedule table failed"));

    // Pick the mode. Master and slave need an LDF to know their slots; without
    // one the device is opened as a monitor, where frames sent from cangaroo
    // still go out once as master header + data.
    LinDb ldb;
    const bool hasLdf = loadLdf(ldb);
    const bool listenOnly = _settings.linListenOnly() ||
                            _settings.linNodeMode() == LinNodeMode::Monitor;

    _mode = Mode::Monitor;
    if (!listenOnly && hasLdf)
    {
        if (_settings.linNodeMode() == LinNodeMode::Master && !ldb.scheduleTableNames().isEmpty())
            _mode = Mode::Master;
        else if (_settings.linNodeMode() == LinNodeMode::Slave)
            _mode = Mode::Slave;
    }
    if (!listenOnly && _mode == Mode::Monitor)
    {
        log_info(tr("uLC: no LDF schedule for %1 mode, opening %2 as monitor")
                     .arg(_settings.linNodeMode() == LinNodeMode::Slave ? "slave" : "master", _name));
    }

    QSet<uint8_t> published;
    QList<QByteArray> slotCmds;
    QByteArray openCmd = ulc::k_cmdOpenMonitor;

    if (_mode == Mode::Master)
    {
        const int tables = static_cast<int>(ldb.scheduleTableNames().size());
        const int table = std::clamp(static_cast<int>(_settings.linScheduleTableIndex()), 0, tables - 1);
        slotCmds = masterTableCommands(ldb, table, published);
        openCmd = ulc::k_cmdOpenMaster;
    }
    else if (_mode == Mode::Slave)
    {
        slotCmds = slaveTableCommands(ldb, published);
        openCmd = ulc::k_cmdOpenSlave;
    }

    // An empty reply is the device's "OK" for mode commands.
    if (transact(openCmd) != QByteArray(""))
    {
        log_error(tr("uLC: %1 did not accept the mode command").arg(_name));
        fail();
        return;
    }

    if (_mode != Mode::Monitor)
    {
        for (const QByteArray &cmd : std::as_const(slotCmds))
        {
            if (!ulc::isFrameAck(transact(cmd).value_or(QByteArray())))
                log_warning(tr("uLC: device rejected slot %1").arg(QString::fromLatin1(cmd.trimmed())));
        }
        if (!ulc::isFrameAck(transact(ulc::k_cmdStartSchedule).value_or(QByteArray())))
            log_warning(tr("uLC: starting the schedule failed"));
    }

    // Reset driver state -- only touched from the BusListener thread hereafter
    _rxLineBuffer.clear();
    _txPendingReply.clear();
    _swallowEmptyId = -1;
    {
        QMutexLocker lock(&_txMutex);
        _txQueue.clear();
        _publishedIds = published;
    }

    _rxCount   = 0;
    _rxErrors  = 0;
    _txCount   = 0;
    _txErrors  = 0;
    _txDropped = 0;
    _state     = state_ok;
    _isOffline = false;
    _isOpen    = true;
}

void UlcInterface::close()
{
    _isOpen = false;
    _state  = state_stopped;

    if (_port && _port->isOpen())
    {
        _port->write(ulc::k_cmdClose);
        _port->waitForBytesWritten(50);
        _port->waitForReadyRead(20);
        _port->clear();
        _port->close();
    }
    _port.reset();

    {
        QMutexLocker lock(&_txMutex);
        _txQueue.clear();
    }
    _txPendingReply.clear();
    _rxLineBuffer.clear();
}

bool UlcInterface::isOpen()
{
    return _isOpen;
}


// ── TX ────────────────────────────────────────────────────────────────────────

// LIN has no free transmit for a master or slave: a node only answers headers
// of the running schedule. There, "sending" sets the payload of a slot this
// node publishes. In monitor mode the device is otherwise silent, so a frame
// goes out once as master header + data (or header only for an RTR message,
// which another node answers).
//
// Commands go out one at a time (see drainTxQueue()), so a sender faster than
// the device must not grow the queue without bound: a payload update replaces
// a queued one for the same ID, and one-shot frames are capped.
void UlcInterface::sendMessage(const BusMessage &msg)
{
    constexpr int k_maxQueuedOneShots = 32;

    const uint8_t id  = static_cast<uint8_t>(msg.getId() & ulc::k_maxLinId);
    const uint8_t len = std::min<uint8_t>(msg.getLength(), ulc::k_maxDataLen);

    QMutexLocker lock(&_txMutex);

    if (_mode == Mode::Monitor)
    {
        const auto queued = std::count_if(_txQueue.cbegin(), _txQueue.cend(),
                                          [](const TxEntry &e) { return e.kind == TxKind::OneShot; });
        if (queued >= k_maxQueuedOneShots)
        {
            _txDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const QByteArray cmd = msg.isRTR() ? ulc::encodeHeader(id, len)
                                           : ulc::encodeFrame(id, msg.getData(), len);
        _txQueue.append(TxEntry{cmd, TxKind::OneShot, msg});
        return;
    }

    if (!_publishedIds.contains(id))
    {
        _txErrors.fetch_add(1, std::memory_order_relaxed);
        log_warning(tr("uLC: LIN ID 0x%1 is not published by this node in the running schedule, data not updated")
                        .arg(id, 2, 16, QLatin1Char('0')));
        return;
    }

    // Only the latest data for a slot matters: replace an update still waiting.
    TxEntry update{ulc::encodeFrame(id, msg.getData(), len), TxKind::PayloadUpdate, msg};
    auto waiting = std::find_if(_txQueue.begin(), _txQueue.end(), [id](const TxEntry &e) {
        return e.kind == TxKind::PayloadUpdate && (e.msg.getId() & ulc::k_maxLinId) == id;
    });
    if (waiting != _txQueue.end())
        *waiting = std::move(update);
    else
        _txQueue.append(std::move(update));

    _sentPayloads.insert(id, QByteArray(reinterpret_cast<const char *>(msg.getData()), len));
}

void UlcInterface::setLinScheduleTable(uint8_t tableIndex)
{
    if (!_isOpen || _mode != Mode::Master)
        return;

    LinDb ldb;
    if (!loadLdf(ldb))
        return;
    if (tableIndex >= static_cast<int>(ldb.scheduleTableNames().size()))
    {
        log_warning(tr("uLC: schedule table %1 out of range").arg(tableIndex));
        return;
    }

    QSet<uint8_t> published;
    const QList<QByteArray> slotCmds = masterTableCommands(ldb, tableIndex, published);

    // The firmware has one table: clear it, load the new slots, restart.
    QMutexLocker lock(&_txMutex);
    _txQueue.append(TxEntry{ulc::k_cmdClearSchedule, TxKind::Control, BusMessage()});
    for (const QByteArray &cmd : slotCmds)
        _txQueue.append(TxEntry{cmd, TxKind::Control, BusMessage()});
    _txQueue.append(TxEntry{ulc::k_cmdStartSchedule, TxKind::Control, BusMessage()});
    _publishedIds = published;
}

// Called from readMessage -- sends the next queued command (no mutex held while writing).
//
// The firmware keeps a single command buffer: a command that arrives before the
// previous one has been answered overwrites it, and is lost. So only one command
// is in flight; the next goes out once the reply arrived or this one timed out.
void UlcInterface::drainTxQueue()
{
    constexpr qint64 k_replyTimeoutMs = 500;

    if (!_txPendingReply.isEmpty())
    {
        if (_replyTimer.elapsed() < k_replyTimeoutMs)
            return;

        const TxEntry lost = _txPendingReply.takeFirst();
        if (lost.kind == TxKind::Control)
            log_warning(tr("uLC: no reply to %1").arg(QString::fromLatin1(lost.cmd.trimmed())));
        else
            _txErrors.fetch_add(1, std::memory_order_relaxed);
    }

    for (;;)
    {
        TxEntry entry;
        {
            QMutexLocker lock(&_txMutex);
            if (_txQueue.isEmpty())
                return;
            entry = _txQueue.takeFirst();
        }

        if (entry.cmd.isEmpty())
        {
            _txDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const qint64 written = _port->write(entry.cmd);
        _port->waitForBytesWritten(50);

        if (written != entry.cmd.size())
        {
            _txErrors.fetch_add(1, std::memory_order_relaxed);
            _txDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        _txPendingReply.append(std::move(entry));
        _replyTimer.start();
        return;
    }
}

void UlcInterface::handleTxReply(QList<BusMessage> &msglist, bool success)
{
    if (_txPendingReply.isEmpty())
        return;

    TxEntry entry = _txPendingReply.takeFirst();

    if (entry.kind == TxKind::Control)
    {
        if (!success)
            log_warning(tr("uLC: device rejected %1").arg(QString::fromLatin1(entry.cmd.trimmed())));
        return;
    }

    if (!success)
    {
        _txErrors.fetch_add(1, std::memory_order_relaxed);
        _state.store(state_tx_fail, std::memory_order_relaxed);
        return;
    }

    _txCount.fetch_add(1, std::memory_order_relaxed);
    _state.store(state_tx_success, std::memory_order_relaxed);

    // A payload update goes out on the slot's next schedule event, which the
    // firmware does not report; only a one-shot frame is on the bus now. A
    // header request (RTR) is shown by the frame the device reports for it.
    if (entry.kind == TxKind::OneShot && !entry.msg.isRTR())
    {
        // Right after acknowledging a one-shot frame the firmware also reports
        // an empty frame with the same ID, as if nobody had answered the header.
        _swallowEmptyId = static_cast<int>(entry.msg.getId() & ulc::k_maxLinId);

        BusMessage txMsg = entry.msg;
        txMsg.setBusType(BusType::LIN);
        txMsg.setInterfaceId(getId());
        txMsg.setRX(false);
        txMsg.setTimestamp_us(nowMicroseconds());
        msglist.append(std::move(txMsg));
    }
}


// ── RX parsing ────────────────────────────────────────────────────────────────

void UlcInterface::handleLine(QList<BusMessage> &msglist)
{
    if (_rxLineBuffer.startsWith('t'))
    {
        BusMessage msg;
        if (!ulc::parseFrameLine(_rxLineBuffer, msg))
        {
            _rxErrors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The echo follows the ack immediately, so only the very next frame
        // line is checked; any other frame ends the wait.
        const bool isEcho = msg.getLength() == 0 &&
                            static_cast<int>(msg.getId()) == _swallowEmptyId;
        _swallowEmptyId = -1;
        if (isEcho)
            return;

        msg.setTimestamp_us(nowMicroseconds());
        msg.setInterfaceId(getId());
        msg.setRX(true);
        if (msg.getLength() == 0)
            _rxErrors.fetch_add(1, std::memory_order_relaxed); // header nobody answered

        msglist.append(std::move(msg));
        _rxCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Replies to queued commands are all frame-type: "z"/"Z" accepts the
    // command, a bare CR means the device had no slot for it.
    if (ulc::isFrameAck(_rxLineBuffer))
        handleTxReply(msglist, true);
    else if (_rxLineBuffer.isEmpty())
        handleTxReply(msglist, false);
    else
        _rxErrors.fetch_add(1, std::memory_order_relaxed);
}


// ── Main read/write loop ──────────────────────────────────────────────────────

bool UlcInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    if (_isOffline.load(std::memory_order_relaxed))
    {
        if (_isOpen)
            close();
        return false;
    }
    if (!_port)
        return false;

    // Send any queued commands before waiting for RX
    drainTxQueue();

    // Block until data arrives (or timeout) instead of busy-spinning. Bytes
    // already buffered (e.g. read during waitForBytesWritten()) count as arrived.
    if (_port->bytesAvailable() == 0 && !_port->waitForReadyRead(static_cast<int>(timeout_ms)))
        return !msglist.isEmpty();

    const QByteArray incoming = _port->readAll();

    for (char c : incoming)
    {
        if (c == '\r')
        {
            handleLine(msglist);
            _rxLineBuffer.clear();
        }
        else if (c == '\x07')
        {
            // BEL = device NACK for a previously sent command
            if (!_rxLineBuffer.isEmpty())
                _rxErrors.fetch_add(1, std::memory_order_relaxed); // partial RX line discarded
            else
                handleTxReply(msglist, false);
            _rxLineBuffer.clear();
        }
        else
        {
            _rxLineBuffer.append(c);
        }
    }

    // Handle a disconnected device (port closed underneath us)
    if (!_port->isOpen())
    {
        _isOffline = true;
        _isOpen    = false;
        _state     = state_bus_off;
        return !msglist.isEmpty();
    }

    // A reply may have freed the device for the next queued command.
    drainTxQueue();

    return !msglist.isEmpty();
}


// ── Statistics ────────────────────────────────────────────────────────────────

bool UlcInterface::updateStatistics()
{
    return false;
}

uint32_t UlcInterface::getState()
{
    return _state.load(std::memory_order_relaxed);
}

int UlcInterface::getNumRxFrames()
{
    return static_cast<int>(_rxCount.load(std::memory_order_relaxed));
}

int UlcInterface::getNumRxErrors()
{
    return _rxErrors.load(std::memory_order_relaxed);
}

int UlcInterface::getNumRxOverruns()
{
    return 0;
}

int UlcInterface::getNumTxFrames()
{
    return static_cast<int>(_txCount.load(std::memory_order_relaxed));
}

int UlcInterface::getNumTxErrors()
{
    return _txErrors.load(std::memory_order_relaxed);
}

int UlcInterface::getNumTxDropped()
{
    return static_cast<int>(_txDropped.load(std::memory_order_relaxed));
}
