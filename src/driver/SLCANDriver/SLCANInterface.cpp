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

#include "SLCANInterface.h"
#include "SLCANDriver.h"

#include <array>
#include <chrono>
#include <span>

#include <QElapsedTimer>

#include <QList>
#include <QString>

#include "core/Log.h"
#include "core/MeasurementInterface.h"

#include "SlcanBitTiming.h"
#include "SlcanFrameCodec.h"

using namespace std::chrono_literals;

static int64_t nowMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct RateEntry { unsigned bitrate; const char *cmd; };

// Nominal bitrate → SLCAN 'S' command, WeAct (enum can_bitrate, WeAct README)
static constexpr std::array<RateEntry, 14> kWeActBitrateTable = {{
    {1000000, "S8"}, {800000,  "S7"}, {500000,  "S6"}, {250000,  "S5"},
    {125000,  "S4"}, {100000,  "S3"}, {83333,   "S9"}, {75000,   "SA"},
    {62500,   "SB"}, {50000,   "S2"}, {33333,   "SC"}, {20000,   "S1"},
    {10000,   "S0"}, {5000,    "SD"},
}};

// CANable (canable-fw / canable2-fw enum can_bitrate): S7 is 750k, not 800k,
// and only CANable 2 knows S9 (83.3k). Which rates are offered per board is
// decided in getAvailableBitrates().
static constexpr std::array<RateEntry, 10> kCanableBitrateTable = {{
    {1000000, "S8"}, {750000,  "S7"}, {500000,  "S6"}, {250000,  "S5"},
    {125000,  "S4"}, {100000,  "S3"}, {83333,   "S9"}, {50000,   "S2"},
    {20000,   "S1"}, {10000,   "S0"},
}};

// FD data bitrate → SLCAN 'Y' command. Elmue has no Y3 (3M); it is not offered there.
static constexpr std::array<RateEntry, 5> kFdBitrateTable = {{
    {1000000, "Y1"}, {2000000, "Y2"}, {3000000, "Y3"},
    {4000000, "Y4"}, {5000000, "Y5"},
}};


// ── Construction ─────────────────────────────────────────────────────────────

SLCANInterface::SLCANInterface(SLCANDriver *driver, int index, QString name,
                               bool fdSupport, Manufacturer manufacturer,
                               SerialTransportFactory transportFactory)
    : BusInterface(reinterpret_cast<CanDriver *>(driver))
    , _manufacturer(manufacturer)
    , _idx(index)
    , _name(std::move(name))
    , _fdSupport(fdSupport)
    , _transportFactory(std::move(transportFactory))
{
    _settings.setBitrate(500000);
    _settings.setSamplePoint(875);

    if (fdSupport)
    {
        _settings.setFdBitrate(2000000);
        _settings.setFdSamplePoint(750);
    }
}

SLCANInterface::~SLCANInterface() = default;


// ── Identity ─────────────────────────────────────────────────────────────────

QString SLCANInterface::getName() const
{
    return _name;
}

void SLCANInterface::setName(QString name)
{
    _name = std::move(name);
}

QString SLCANInterface::getDetailsStr() const
{
    const bool fd = _fdSupport;
    if (isElmue())
        return tr("CANable 2.5 (ElmueSoft Slcan firmware) with CANFD support");
    switch (_manufacturer)
    {
        case Manufacturer::CANable:
            return fd ? tr("CANable with CANFD support")
                      : tr("CANable with standard CAN support");
        case Manufacturer::WeActStudio:
            return fd ? tr("WeAct Studio USB2CAN with CANFD support")
                      : tr("WeAct Studio USB2CAN with standard CAN support");
        case Manufacturer::Elmue:
            return tr("CANable 2.5 (ElmueSoft Slcan firmware) with CANFD support");
    }
    return tr("SLCAN device");
}

QString SLCANInterface::getVersion()
{
    return _version.isEmpty() ? BusInterface::getVersion() : _version;
}

int SLCANInterface::getIfIndex() const noexcept
{
    return _idx;
}


// ── Configuration ─────────────────────────────────────────────────────────────

QList<CanTiming> SLCANInterface::getAvailableBitrates()
{
    QList<unsigned> bitrates;
    QList<unsigned> fdBitrates;

    switch (_manufacturer)
    {
        case Manufacturer::CANable:
            // CANable 1 (no FD) has 750k but no 83.3k. CANable 2 has 83.3k; its
            // 750k prescaler gives about 1.43 Mbit/s ("VERY INACCURATE"), so it
            // is not offered.
            if (_fdSupport)
                bitrates = {10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 1000000};
            else
                bitrates = {10000, 20000, 50000, 100000, 125000, 250000, 500000, 750000, 1000000};
            fdBitrates   = {2000000, 5000000};
            break;
        case Manufacturer::WeActStudio:
            bitrates     = {5000, 10000, 20000, 33333, 50000, 62500, 75000, 83333,
                            100000, 125000, 250000, 500000, 800000, 1000000};
            fdBitrates   = {1000000, 2000000, 3000000, 4000000, 5000000};
            break;
        case Manufacturer::Elmue:
            // Slcan/control.h: the WeAct letters S0-SD; Y0/1/2/4/5/8, of which
            // 8M only on STM32G4 and 500k is below what a setup page offers.
            bitrates     = {5000, 10000, 20000, 33333, 50000, 62500, 75000, 83333,
                            100000, 125000, 250000, 500000, 800000, 1000000};
            fdBitrates   = {1000000, 2000000, 4000000, 5000000};
            break;
    }

    QList<CanTiming> result;
    unsigned i = 0;
    for (unsigned br : std::as_const(bitrates))
        for (unsigned brFd : std::as_const(fdBitrates))
            result << CanTiming(i++, br, brFd, 875, 750);

    return result;
}

void SLCANInterface::applyConfig(const MeasurementInterface &mi)
{
    _settings = mi;
}

unsigned SLCANInterface::getBitrate()
{
    return _settings.bitrate();
}

uint32_t SLCANInterface::getCapabilities()
{
    uint32_t caps = 0;

    switch (_manufacturer)
    {
        case Manufacturer::CANable:
            // No listen only: both CANable firmwares accept "M1", but
            // can_enable() then opens the channel in normal mode anyway.
            caps = capability_config_os | capability_auto_restart;
            break;
        case Manufacturer::WeActStudio:
            caps = capability_listen_only | capability_custom_bitrate | capability_custom_canfd_bitrate;
            break;
        case Manufacturer::Elmue:
            break;
    }

    // Elmue's "M1" opens in bus monitoring mode, unlike the stock CANable
    // firmware. Custom timings use another format there ("s"/"y", decimal).
    if (isElmue())
        caps = (caps & ~capability_custom_bitrate & ~capability_custom_canfd_bitrate) | capability_listen_only;

    if (_fdSupport)
        caps |= capability_canfd;

    return caps;
}


// ── Serial helpers ────────────────────────────────────────────────────────────

void SLCANInterface::writePort(const char *cmd)
{
    writePort(QByteArray(cmd));
}

void SLCANInterface::writePort(const QByteArray &data)
{
    _port->write(data);
    _port->waitForBytesWritten(50ms);
}

bool SLCANInterface::isElmue() const
{
    return _manufacturer == Manufacturer::Elmue || _elmueFirmware.load(std::memory_order_relaxed);
}

QByteArray SLCANInterface::elmueTimingCommand(bool data) const
{
    if (!isElmue() || _elmueClockHz == 0)
        return {};

    const unsigned bitrate = data ? _settings.fdBitrate() : _settings.bitrate();
    const int samplePoint = data ? static_cast<int>(_settings.fdSamplePoint()) : _settings.samplePoint();
    const size_t base = data ? 4 : 0;
    const slcan::TimingLimits lim{.brpMax = _elmueLimits[base], .seg1Max = _elmueLimits[base + 1],
                                  .seg2Max = _elmueLimits[base + 2], .sjwMax = _elmueLimits[base + 3]};

    const auto t = slcan::calcBitTiming(_elmueClockHz, bitrate, static_cast<uint32_t>(samplePoint), lim);
    if (!t)
        return {};
    return QStringLiteral("%1%2,%3,%4,%5\r")
        .arg(data ? 'y' : 's').arg(t->brp).arg(t->seg1).arg(t->seg2).arg(t->sjw)
        .toLatin1();
}

bool SLCANInterface::sendConfig(const QByteArray &cmd)
{
    writePort(cmd);
    if (_noConfirm.load(std::memory_order_relaxed))
        return true;

    // Only setup commands are in flight here and the channel is closed, so
    // the first CR or BEL is this command's answer.
    constexpr qint64 k_timeoutMs = 100;
    QElapsedTimer timer;
    timer.start();
    for (;;)
    {
        const QByteArray reply = _port->readAll();
        const int cr = reply.indexOf('\r');
        const int bel = reply.indexOf('\x07');
        if (bel >= 0 && (cr < 0 || bel < cr))
        {
            log_warning(tr("SLCAN: %1 refused \"%2\"").arg(_name, QString::fromLatin1(cmd.trimmed())));
            return false;
        }
        if (cr >= 0)
            return true;

        const qint64 remaining = k_timeoutMs - timer.elapsed();
        if (remaining <= 0 || !_port->waitForReadyRead(std::chrono::milliseconds(remaining)))
            break;
    }
    log_warning(tr("SLCAN: %1 did not answer \"%2\"").arg(_name, QString::fromLatin1(cmd.trimmed())));
    return false;
}

void SLCANInterface::configureBitrate()
{
    if (_settings.isCustomBitrate())
    {
        QByteArray cmd = "S";
        cmd += QString::number(_settings.customBitrate(), 16).toUpper().rightJustified(6, '0').toLatin1();
        cmd += '\r';
        sendConfig(cmd);
        return;
    }

    // Elmue: exact timing at the configured sample point, see SlcanBitTiming.h
    if (const QByteArray timing = elmueTimingCommand(false); !timing.isEmpty() && sendConfig(timing))
        return;

    const std::span<const RateEntry> table = (_manufacturer == Manufacturer::WeActStudio || isElmue())
        ? std::span<const RateEntry>(kWeActBitrateTable)
        : std::span<const RateEntry>(kCanableBitrateTable);
    for (const auto &entry : table)
    {
        if (entry.bitrate == _settings.bitrate())
        {
            QByteArray cmd = entry.cmd;
            cmd += '\r';
            sendConfig(cmd);
            return;
        }
    }
    // Fallback: 500 kbit/s
    sendConfig("S6\r");
}

void SLCANInterface::configureFdBitrate()
{
    if (_settings.isCustomFdBitrate())
    {
        QByteArray cmd = "Y";
        cmd += QString::number(_settings.customFdBitrate(), 16).toUpper().rightJustified(6, '0').toLatin1();
        cmd += '\r';
        sendConfig(cmd);
        return;
    }

    if (const QByteArray timing = elmueTimingCommand(true); !timing.isEmpty() && sendConfig(timing))
        return;

    for (const auto &entry : kFdBitrateTable)
    {
        if (entry.bitrate == _settings.fdBitrate() && !(isElmue() && entry.bitrate == 3000000))
        {
            QByteArray cmd = entry.cmd;
            cmd += '\r';
            sendConfig(cmd);
            return;
        }
    }
    // Fallback: 2 Mbit/s
    sendConfig("Y2\r");
}


// ── Open / Close ──────────────────────────────────────────────────────────────

// Brings the device into a known state (ASCII mode, channel closed) and reads
// what it says about itself: confirmations, firmware version, ElmueSoft limits.
// Used by open() and by probe(); the results stay valid after close().
void SLCANInterface::queryDevice()
{
    // WeAct devices keep binary mode (H1) set by another application across
    // sessions; this driver only speaks ASCII, so force text mode first (issue #43).
    if (_manufacturer == Manufacturer::WeActStudio)
    {
        writePort("H0\r");
        if (_port->waitForReadyRead(50ms))
            (void)_port->readAll();
        _port->clear();
    }

    // Close a channel an earlier session may have left open. Its bus traffic
    // can still be on the way, so wait until the line is quiet (bounded, so a
    // device that never stops sending cannot hang open()).
    writePort("C\r");
    for (int i = 0; i < 20 && _port->waitForReadyRead(50ms); ++i)
        (void)_port->readAll();
    _port->clear();

    // Probe whether the device sends confirmations: on the closed channel only
    // a confirming device (WeAct) answers "C\r", with CR. CANable stays silent.
    writePort("C\r");
    _noConfirm = !_port->waitForReadyRead(50ms);
    (void)_port->readAll(); // discard response

    // Read the firmware version, up to its CR: a long one (Elmue, ~200 bytes)
    // takes several USB packets.
    _port->clear();
    writePort("V\r");
    QByteArray version;
    for (int i = 0; i < 10 && !version.contains('\r') && _port->waitForReadyRead(50ms); ++i)
        version += _port->readAll();
    version += _port->readAll();
    _version = QString::fromLatin1(version.left(version.indexOf('\r'))).trimmed();

    // ElmueSoft firmware ("+Board: Multiboard\tMCU: STM32G431\t...") answers every
    // command but "C", so the probe above took it for a silent CANable.
    _elmueFirmware = _version.startsWith(QStringLiteral("+Board:"));
    _elmueClockHz = 0;
    if (_elmueFirmware)
    {
        // "...\tClock: 160\t...\tLimits: 512,256,128,128,32,32,16,16\t..." (MHz;
        // nominal BRP, Seg1, Seg2, SJW maxima, then the data phase ones)
        for (const QString &field : _version.split('\t'))
        {
            if (field.startsWith(QStringLiteral("Clock: ")))
                _elmueClockHz = field.mid(7).toUInt() * 1000000u;
            else if (field.startsWith(QStringLiteral("Limits: ")))
            {
                const QStringList values = field.mid(8).split(',');
                if (values.size() != static_cast<qsizetype>(_elmueLimits.size()))
                    _elmueClockHz = 0;  // unknown layout: presets only
                for (int i = 0; i < values.size() && i < static_cast<int>(_elmueLimits.size()); ++i)
                    _elmueLimits[static_cast<size_t>(i)] = values[i].toUInt();
            }
        }
        if (std::find(_elmueLimits.cbegin(), _elmueLimits.cend(), 0u) != _elmueLimits.cend())
            _elmueClockHz = 0;
    }
    if (isElmue())
    {
        _noConfirm = false;
        _version = _version.mid(1).replace('\t', QStringLiteral(", "));
    }
}

bool SLCANInterface::probe()
{
    if (_isOpen)
        return true;

    _port = _transportFactory();
    if (!_port->open(_name, SerialTransport::Settings{.baudRate = 1000000}))
    {
        _port.reset();
        return false;
    }
    _port->clear();
    queryDevice();
    _port->clear();
    _port->close();
    _port.reset();
    return !_version.isEmpty();
}


void SLCANInterface::open()
{
    _port = _transportFactory();

    if (!_port->open(_name, SerialTransport::Settings{.baudRate = 1000000}))
    {
        log_error(tr("SLCAN: failed to open %1: %2").arg(_name, _port->errorString()));
        _port.reset();
        _isOpen = false;
        _isOffline = true;
        return;
    }
    _port->clear();

    queryDevice();

    configureBitrate();
    if (_fdSupport)
        configureFdBitrate();

    if (_settings.isListenOnlyMode() && !(getCapabilities() & capability_listen_only))
    {
        // A setup saved before the option was disabled for this adapter
        log_warning(tr("SLCAN: %1 cannot run listen only, the channel opens in normal mode "
                       "and acknowledges frames").arg(_name));
    }
    sendConfig(_settings.isListenOnlyMode() ? "M1\r" : "M0\r");
    if (!sendConfig("O\r"))
    {
        // Only a confirming device can say so; it did not open the channel.
        log_error(tr("SLCAN: %1 did not open the CAN channel").arg(_name));
        _port->close();
        _port.reset();
        _isOpen = false;
        _isOffline = true;
        return;
    }

    // Discard any immediate responses from opening
    if (_port->waitForReadyRead(10ms))
        (void)_port->readAll();

    // Reset driver state — only touched from this thread hereafter
    _rxLineBuffer.clear();
    _txPendingConfirm.clear();

    {
        QMutexLocker lock(&_txMutex);
        _txQueue.clear();
    }

    _rxCount   = 0;
    _rxErrors  = 0;
    _rxOverruns = 0;
    _txCount   = 0;
    _txErrors  = 0;
    _txDropped = 0;
    _state     = state_ok;
    _isOffline = false;
    _isOpen    = true;
}

void SLCANInterface::close()
{
    _isOpen = false;
    _state  = state_bus_off;

    if (_port && _port->isOpen())
    {
        writePort("C\r");
        _port->waitForReadyRead(20ms);
        _port->clear();
        _port->close();
    }
    _port.reset();

    {
        QMutexLocker lock(&_txMutex);
        _txQueue.clear();
    }
    _txPendingConfirm.clear();
    _rxLineBuffer.clear();
}

bool SLCANInterface::isOpen()
{
    return _isOpen;
}


// ── TX ────────────────────────────────────────────────────────────────────────

// Encodes a BusMessage into an SLCAN ASCII frame (including trailing CR).
QByteArray SLCANInterface::encodeFrame(const BusMessage &msg)
{
    const bool isExtended = msg.isExtended();
    const bool isFd       = msg.isFD();
    const bool isBrs      = msg.isBRS();
    const bool isRtr      = msg.isRTR();
    const int  dataLen    = msg.getLength();

    if (dataLen < 0 || dataLen > 64)
        return {};
    if (isFd && isRtr) // CAN-FD has no RTR frames
        return {};

    // Upper case means a 29-bit ID, for FD frames too: "b"/"d" are 11-bit and
    // "B"/"D" 29-bit in both the CANable 2 and the WeAct firmware.
    char typeChar;
    if (isFd)
        typeChar = isBrs ? 'b' : 'd';
    else if (isRtr)
        typeChar = 'r';
    else
        typeChar = 't';
    if (isExtended)
        typeChar = static_cast<char>(typeChar - ('a' - 'A'));

    // A remote frame's DLC says how much data is requested; no data follows
    // it (WeAct rejects a line with payload digits).
    const int payloadLen = isRtr ? 0 : dataLen;

    const int idLen = isExtended ? slcan::k_extIdLen : slcan::k_stdIdLen;

    QByteArray frame;
    frame.reserve(1 + idLen + 1 + payloadLen * 2 + 1);
    frame.append(typeChar);

    // ID, MSB-first
    uint32_t id = msg.getId();
    char idBuf[slcan::k_extIdLen];
    for (int i = idLen - 1; i >= 0; --i)
    {
        idBuf[i] = slcan::hexNibble(static_cast<uint8_t>(id & 0xF));
        id >>= 4;
    }
    frame.append(idBuf, idLen);

    // DLC nibble
    frame.append(slcan::hexNibble(slcan::bytesToDlcNibble(dataLen)));

    // Data bytes
    for (int i = 0; i < payloadLen; ++i)
    {
        const uint8_t b = msg.getByte(i);
        frame.append(slcan::hexNibble(b >> 4));
        frame.append(slcan::hexNibble(b & 0x0F));
    }

    frame.append('\r');
    return frame;
}

void SLCANInterface::sendMessage(const BusMessage &msg)
{
    QByteArray frame = encodeFrame(msg);
    if (frame.isEmpty())
        return;

    QMutexLocker lock(&_txMutex);
    _txQueue.append(TxEntry{std::move(frame), msg});
}

// Called from readMessage — drains the TX queue (no mutex held while writing).
void SLCANInterface::drainTxQueue(QList<BusMessage> &msglist)
{
    QList<TxEntry> toSend;
    {
        QMutexLocker lock(&_txMutex);
        toSend = std::move(_txQueue);
    }

    for (auto &entry : toSend)
    {
        const qint64 written = _port->write(entry.frame);
        _port->waitForBytesWritten(50ms);

        if (written != entry.frame.size())
        {
            _txErrors.fetch_add(1, std::memory_order_relaxed);
            _txDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (_noConfirm.load(std::memory_order_relaxed))
        {
            // Device does not send confirmations — report TX immediately
            BusMessage txMsg = entry.msg;
            txMsg.setRX(false);
            txMsg.setTimestamp_us(nowMicroseconds());
            msglist.append(std::move(txMsg));
            _txCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            _txPendingConfirm.append(std::move(entry.msg));
        }
    }
}

void SLCANInterface::handleTxConfirm(QList<BusMessage> &msglist, bool success)
{
    if (_txPendingConfirm.isEmpty())
        return;

    BusMessage txMsg = _txPendingConfirm.takeFirst();
    txMsg.setRX(false);

    if (success)
    {
        txMsg.setTimestamp_us(nowMicroseconds());
        msglist.append(std::move(txMsg));
        _txCount.fetch_add(1, std::memory_order_relaxed);
        _state.store(state_tx_success, std::memory_order_relaxed);
    }
    else
    {
        _txErrors.fetch_add(1, std::memory_order_relaxed);
        _state.store(state_tx_fail, std::memory_order_relaxed);
    }
}


// ── RX parsing ────────────────────────────────────────────────────────────────

bool SLCANInterface::parseRxLine(QList<BusMessage> &msglist)
{
    // Frame decoding itself lives in SlcanFrameCodec.h so it can be unit
    // tested; this wrapper only adds the parts that depend on interface state.
    BusMessage msg;
    if (!slcan::parseFrameLine(_rxLineBuffer, msg))
    {
        _rxErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    msg.setTimestamp_us(nowMicroseconds());
    msg.setInterfaceId(getId());
    msg.setRX(true);

    msglist.append(std::move(msg));
    _rxCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}


// ── Main read/write loop ──────────────────────────────────────────────────────

bool SLCANInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    if (_isOffline.load(std::memory_order_relaxed))
    {
        if (_isOpen)
            close();
        return false;
    }

    // Send any queued TX frames before waiting for RX
    drainTxQueue(msglist);

    // Block until data arrives (or timeout) instead of busy-spinning. Bytes
    // already buffered count as arrived: waitForBytesWritten() in drainTxQueue()
    // also reads, and waitForReadyRead() only waits for newer bytes.
    if (_port->bytesAvailable() == 0 &&
        !_port->waitForReadyRead(std::chrono::milliseconds(timeout_ms)))
    {
        // A plain timeout is the idle case; anything else means the port is gone.
        checkPortLost();
        return !msglist.isEmpty();
    }

    const QByteArray incoming = _port->readAll();

    for (char c : incoming)
    {
        if (c == '\r')
        {
            if (_rxLineBuffer.isEmpty())
            {
                // Bare CR = device ACK for a previously sent frame
                if (!_noConfirm.load(std::memory_order_relaxed))
                    handleTxConfirm(msglist, true);
            }
            else
            {
                parseRxLine(msglist);
            }
            _rxLineBuffer.clear();
        }
        else if (c == '\x07')
        {
            // BEL = device NACK for a previously sent frame
            if (!_rxLineBuffer.isEmpty())
                _rxErrors.fetch_add(1, std::memory_order_relaxed); // partial RX frame discarded
            else if (!_noConfirm.load(std::memory_order_relaxed))
                handleTxConfirm(msglist, false);
            _rxLineBuffer.clear();
        }
        else
        {
            _rxLineBuffer.append(c);
        }
    }

    checkPortLost();
    return !msglist.isEmpty();
}

// An unplugged adapter leaves the serial port open and only reports an error
// (ResourceError on Linux, others elsewhere), so isOpen() alone misses it.
bool SLCANInterface::checkPortLost()
{
    if (_port->isOpen() && !_port->hasFailed())
        return false;

    log_error(tr("SLCAN: connection to %1 lost (%2)").arg(_name, _port->errorString()));
    _isOffline = true;
    _isOpen    = false;
    _state     = state_bus_off;
    return true;
}


// ── Statistics ────────────────────────────────────────────────────────────────

bool SLCANInterface::updateStatistics()
{
    return false;
}

uint32_t SLCANInterface::getState()
{
    return _state.load(std::memory_order_relaxed);
}

int SLCANInterface::getNumRxFrames()
{
    return static_cast<int>(_rxCount.load(std::memory_order_relaxed));
}

int SLCANInterface::getNumRxErrors()
{
    return _rxErrors.load(std::memory_order_relaxed);
}

int SLCANInterface::getNumRxOverruns()
{
    return static_cast<int>(_rxOverruns.load(std::memory_order_relaxed));
}

int SLCANInterface::getNumTxFrames()
{
    return static_cast<int>(_txCount.load(std::memory_order_relaxed));
}

int SLCANInterface::getNumTxErrors()
{
    return _txErrors.load(std::memory_order_relaxed);
}

int SLCANInterface::getNumTxDropped()
{
    return static_cast<int>(_txDropped.load(std::memory_order_relaxed));
}
