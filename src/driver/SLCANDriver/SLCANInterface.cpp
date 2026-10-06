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

#include <QList>
#include <QString>

#include "core/Backend.h"
#include "core/MeasurementInterface.h"

#include "SlcanFrameCodec.h"

static int64_t nowMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct RateEntry { unsigned bitrate; const char *cmd; };

// Nominal bitrate → SLCAN 'S' command
static constexpr std::array<RateEntry, 14> kBitrateTable = {{
    {1000000, "S8"}, {800000,  "S7"}, {500000,  "S6"}, {250000,  "S5"},
    {125000,  "S4"}, {100000,  "S3"}, {83333,   "S9"}, {75000,   "SA"},
    {62500,   "SB"}, {50000,   "S2"}, {33333,   "SC"}, {20000,   "S1"},
    {10000,   "S0"}, {5000,    "SD"},
}};

// FD data bitrate → SLCAN 'Y' command
static constexpr std::array<RateEntry, 5> kFdBitrateTable = {{
    {1000000, "Y1"}, {2000000, "Y2"}, {3000000, "Y3"},
    {4000000, "Y4"}, {5000000, "Y5"},
}};


// ── Construction ─────────────────────────────────────────────────────────────

SLCANInterface::SLCANInterface(SLCANDriver *driver, int index, QString name,
                               bool fdSupport, Manufacturer manufacturer)
    : BusInterface(reinterpret_cast<CanDriver *>(driver))
    , _manufacturer(manufacturer)
    , _idx(index)
    , _name(std::move(name))
    , _fdSupport(fdSupport)
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
    switch (_manufacturer)
    {
        case Manufacturer::CANable:
            return fd ? tr("CANable with CANFD support")
                      : tr("CANable with standard CAN support");
        case Manufacturer::WeActStudio:
            return fd ? tr("WeAct Studio USB2CAN with CANFD support")
                      : tr("WeAct Studio USB2CAN with standard CAN support");
    }
    return tr("SLCAN device");
}

QString SLCANInterface::getVersion()
{
    return _version;
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
            bitrates     = {10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000};
            fdBitrates   = {2000000, 5000000};
            break;
        case Manufacturer::WeActStudio:
            bitrates     = {5000, 10000, 20000, 33333, 50000, 62500, 75000, 83333,
                            100000, 125000, 250000, 500000, 800000, 1000000};
            fdBitrates   = {1000000, 2000000, 3000000, 4000000, 5000000};
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
            caps = capability_config_os | capability_auto_restart | capability_listen_only;
            break;
        case Manufacturer::WeActStudio:
            caps = capability_listen_only | capability_custom_bitrate | capability_custom_canfd_bitrate;
            break;
    }

    if (_fdSupport)
        caps |= capability_canfd;

    return caps;
}


// ── Serial helpers ────────────────────────────────────────────────────────────

void SLCANInterface::writePort(const char *cmd)
{
    _port->write(cmd);
    _port->waitForBytesWritten(50);
}

void SLCANInterface::writePort(const QByteArray &data)
{
    _port->write(data);
    _port->waitForBytesWritten(50);
}

void SLCANInterface::configureBitrate()
{
    if (_settings.isCustomBitrate())
    {
        QByteArray cmd = "S";
        cmd += QString::number(_settings.customBitrate(), 16).toUpper().rightJustified(6, '0').toLatin1();
        cmd += '\r';
        writePort(cmd);
        return;
    }

    for (const auto &entry : kBitrateTable)
    {
        if (entry.bitrate == _settings.bitrate())
        {
            QByteArray cmd = entry.cmd;
            cmd += '\r';
            writePort(cmd);
            return;
        }
    }
    // Fallback: 500 kbit/s
    writePort("S6\r");
}

void SLCANInterface::configureFdBitrate()
{
    if (_settings.isCustomFdBitrate())
    {
        QByteArray cmd = "Y";
        cmd += QString::number(_settings.customFdBitrate(), 16).toUpper().rightJustified(6, '0').toLatin1();
        cmd += '\r';
        writePort(cmd);
        return;
    }

    for (const auto &entry : kFdBitrateTable)
    {
        if (entry.bitrate == _settings.fdBitrate())
        {
            QByteArray cmd = entry.cmd;
            cmd += '\r';
            writePort(cmd);
            return;
        }
    }
    // Fallback: 2 Mbit/s
    writePort("Y2\r");
}


// ── Open / Close ──────────────────────────────────────────────────────────────

void SLCANInterface::open()
{
    _port = std::make_unique<QSerialPort>();
    _port->setPortName(_name);
    _port->setBaudRate(1000000);
    _port->setDataBits(QSerialPort::Data8);
    _port->setParity(QSerialPort::NoParity);
    _port->setStopBits(QSerialPort::OneStop);
    _port->setFlowControl(QSerialPort::NoFlowControl);

    if (!_port->open(QIODevice::ReadWrite))
    {
        log_error(tr("SLCAN: failed to open %1: %2").arg(_name, _port->errorString()));
        _port.reset();
        _isOpen = false;
        _isOffline = true;
        return;
    }
    _port->clear();

    // WeAct devices keep binary mode (H1) set by another application across
    // sessions; this driver only speaks ASCII, so force text mode first (issue #43).
    if (_manufacturer == Manufacturer::WeActStudio)
    {
        writePort("H0\r");
        if (_port->waitForReadyRead(50))
            _port->readAll();
        _port->clear();
    }

    // Close any open CAN channel, then probe whether device sends confirmations.
    // A device that responds to "C\r" with CR is in confirm mode.
    writePort("C\r");
    _noConfirm = !_port->waitForReadyRead(50);
    _port->readAll(); // discard response

    // Read firmware version
    _port->clear();
    writePort("V\r");
    if (_port->waitForReadyRead(50))
    {
        QByteArray resp = _port->readLine();
        _version = QString::fromLatin1(resp).trimmed();
    }

    configureBitrate();
    if (_fdSupport)
        configureFdBitrate();

    writePort(_settings.isListenOnlyMode() ? "M1\r" : "M0\r");
    writePort("O\r");

    // Discard any immediate responses from opening
    if (_port->waitForReadyRead(10))
        _port->readAll();

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
        _port->waitForReadyRead(20);
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

    char typeChar;
    if (isFd)
        // FD type chars are always lowercase regardless of ID length (firmware convention)
        typeChar = isBrs ? 'b' : 'd';
    else if (isRtr)
        typeChar = isExtended ? 'R' : 'r';
    else
        typeChar = isExtended ? 'T' : 't';

    const int idLen = isExtended ? slcan::k_extIdLen : slcan::k_stdIdLen;

    QByteArray frame;
    frame.reserve(1 + idLen + 1 + dataLen * 2 + 1);
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
    for (int i = 0; i < dataLen; ++i)
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
        _port->waitForBytesWritten(50);

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

    // Block until data arrives (or timeout). This yields the BusListener thread
    // efficiently rather than busy-spinning.
    if (!_port->waitForReadyRead(static_cast<int>(timeout_ms)))
        return !msglist.isEmpty();

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

    // Handle a disconnected device (port closed underneath us)
    if (!_port->isOpen())
    {
        _isOffline = true;
        _isOpen    = false;
        _state     = state_bus_off;
    }

    return !msglist.isEmpty();
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
