/*

  Copyright (c) 2022 Ethan Zonca

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

#include "CANBlasterInterface.h"

#include <algorithm>
#include <cstdio>

#include <QNetworkDatagram>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>

#include "core/Backend.h"
#include "core/Log.h"
#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"


CANBlasterInterface::CANBlasterInterface(CANBlasterDriver *driver, int index, QString name, bool fd_support)
  : BusInterface(reinterpret_cast<CanDriver*>(driver)),
    _idx(index),
    _name(name),
    _ts_mode(ts_mode_SIOCSHWTSTAMP)
{
    // Set defaults
    _settings.setBitrate(500000);
    _settings.setSamplePoint(875);

    _config.supports_canfd = fd_support;

    // Record start time
    _heartbeat_time_ms = QDateTime::currentMSecsSinceEpoch();
}

CANBlasterInterface::~CANBlasterInterface()
{
    close();  // releases the client port for the next interface
}

QString CANBlasterInterface::getDetailsStr() const {
    if(_config.supports_canfd)
    {
        return tr("CANBlaster client with CANFD support");
    }
    else
    {
        return tr("CANBlaster client with standard CAN support");
    }
}

QString CANBlasterInterface::getName() const {
    return _name;
}

void CANBlasterInterface::setName(QString name) {
    _name = name;
}

QList<CanTiming> CANBlasterInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    QList<unsigned> bitrates({10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000});
    QList<unsigned> bitrates_fd({0, 2000000, 5000000});

    QList<unsigned> samplePoints({875});

    unsigned i=0;
    for (unsigned br : bitrates) {
        for (unsigned br_fd : bitrates_fd) {
            for (unsigned sp : samplePoints) {
                retval << CanTiming(i++, br, br_fd, sp);
            }
        }
    }

    return retval;
}


void CANBlasterInterface::applyConfig(const MeasurementInterface &mi)
{
    // Save settings for port configuration
    _settings = mi;
}



bool CANBlasterInterface::supportsTimingConfiguration()
{
    return _config.supports_timing;
}

bool CANBlasterInterface::supportsCanFD()
{
    return _config.supports_canfd;
}

bool CANBlasterInterface::supportsTripleSampling()
{
    return false;
}

unsigned CANBlasterInterface::getBitrate()
{
    return 0;
}

uint32_t CANBlasterInterface::getCapabilities()
{
    uint32_t retval =
        BusInterface::capability_config_os |
        BusInterface::capability_listen_only |
        BusInterface::capability_auto_restart;

    if (supportsTripleSampling()) {
        retval |= BusInterface::capability_triple_sampling;
    }

    return retval;
}

bool CANBlasterInterface::updateStatistics()
{
    return false;
}

uint32_t CANBlasterInterface::getState()
{
    if(_isOpen)
        return state_ok;
    else
        return state_bus_off;
}

int CANBlasterInterface::getNumRxFrames()
{
    return static_cast<int>(_status.rx_count.load());
}

int CANBlasterInterface::getNumRxErrors()
{
    return _status.rx_errors.load();
}

int CANBlasterInterface::getNumTxFrames()
{
    return static_cast<int>(_status.tx_count.load());
}

int CANBlasterInterface::getNumTxErrors()
{
    return _status.tx_errors.load();
}

int CANBlasterInterface::getNumRxOverruns()
{
    return static_cast<int>(_status.rx_overruns.load());
}

int CANBlasterInterface::getNumTxDropped()
{
    return static_cast<int>(_status.tx_dropped.load());
}

int CANBlasterInterface::getIfIndex() {
    return _idx;
}


void CANBlasterInterface::open()
{
    // Start off with a fresh socket
    _socket = std::make_unique<QUdpSocket>();

    // canblaster.py sends a client the frames from its heartbeat's source
    // address, so heartbeats and frames share this port.
    if (_socket->bind(QHostAddress::AnyIPv4, 20001))
    {
        _isOpen = true;
        sendHeartbeat();  // subscribe now, not a second later
    }
    else
    {
        log_error(tr("CANblaster: cannot listen on UDP port 20001: %1").arg(_socket->errorString()));
        _socket.reset();
        _isOpen = false;
    }
}

void CANBlasterInterface::close()
{
    _socket.reset();
    _isOpen = false;
}

bool CANBlasterInterface::isOpen()
{
    return _isOpen;
}

void CANBlasterInterface::sendMessage(const BusMessage &msg)
{
    // canblaster.py does not put frames from clients on the bus (TX is a TODO there)
    Q_UNUSED(msg);
}

void CANBlasterInterface::sendHeartbeat()
{
    // Any datagram is a heartbeat; the server forgets a client after 5 s without one
    _heartbeat_time_ms = QDateTime::currentMSecsSinceEpoch();
    _socket->writeDatagram(QByteArrayLiteral("Heartbeat"), QHostAddress(getName()), 20002);
}

bool CANBlasterInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    if (!_isOpen || !_socket)
        return false;

    if (QDateTime::currentMSecsSinceEpoch() - _heartbeat_time_ms > 1000)
        sendHeartbeat();

    // Wait for data instead of returning at once: the listener calls this in a loop.
    if (!_socket->hasPendingDatagrams() && !_socket->waitForReadyRead(static_cast<int>(timeout_ms)))
        return false;

    // Every pending datagram: at full bus load the server sends thousands per
    // second, and the socket buffer overflows if they are read one per call.
    const QHostAddress server(getName());
    bool any = false;
    while (_socket->hasPendingDatagrams())
    {
        can_frame frame{};
        QHostAddress address;
        const qint64 res = _socket->readDatagram(reinterpret_cast<char *>(&frame), sizeof(frame), &address);

        // Only the server's classic frames (CAN_MTU); not a stray datagram on this port
        if (res != static_cast<qint64>(sizeof(can_frame)) || !address.isEqual(server, QHostAddress::TolerantConversion))
            continue;

        BusMessage msg;
        msg.setTimestamp_ms(QDateTime::currentMSecsSinceEpoch());
        msg.setInterfaceId(getId());
        msg.setId(frame.can_id & CAN_ERR_MASK);
        msg.setBRS(false);
        msg.setErrorFrame(frame.can_id & CAN_ERR_FLAG);
        msg.setExtended(frame.can_id & CAN_EFF_FLAG);
        msg.setRTR(frame.can_id & CAN_RTR_FLAG);
        const uint8_t len = std::min<uint8_t>(frame.len, CAN_MAX_DLEN);
        msg.setLength(len);
        for (uint8_t i = 0; i < len; i++)
            msg.setDataAt(i, frame.data[i]);
        msglist.append(msg);
        _status.rx_count.fetch_add(1);
        any = true;
    }
    return any;
}

// Listens for the multicast beacons of CANblaster servers for `window` and
// returns their addresses (canblaster.py send_beacon()).
QList<QString> CANBlasterInterface::discoverServers(std::chrono::milliseconds window)
{
    // TODO: Listen for multicast packets for discovery of canblaster servers
    QUdpSocket udpSocket;
    QHostAddress groupAddress;
    groupAddress.setAddress("239.255.43.21");
    udpSocket.bind(QHostAddress::AnyIPv4, 20000, QUdpSocket::ShareAddress);
    udpSocket.joinMulticastGroup(groupAddress);

    // Record start time
    qint64 start_time_ms = QDateTime::currentMSecsSinceEpoch();

    fprintf(stderr, "CANblaster: start listen\r\n");

    QList<QString> detected_servers;

    while(1)
    {
        // Wait for a datagram instead of polling (the window blocks the caller)
        const qint64 left = window.count() - (QDateTime::currentMSecsSinceEpoch() - start_time_ms);
        if (left > 0 && !udpSocket.hasPendingDatagrams())
            udpSocket.waitForReadyRead(static_cast<int>(left));

        while (udpSocket.hasPendingDatagrams())
        {
            QNetworkDatagram res = udpSocket.receiveDatagram(1024);
            if(res.isValid())
            {
                QByteArray asd = res.data();
                qDebug() << asd;
                QJsonDocument document = QJsonDocument::fromJson(res.data());
                QJsonObject rootObj = document.object();


                if(rootObj.length() == 2 &&
                   rootObj["protocol"].toString() == "CANblaster" &&
                   rootObj["version"].toInt() == 1)
                {
                    if(!detected_servers.contains(res.senderAddress().toString()))
                        detected_servers.append(res.senderAddress().toString());
                }
                else
                {
                    fprintf(stderr, "Invalid CANblaster server. Protocol: %s  Version: %d \r\n", rootObj["protocol"].toString().toStdString().c_str(), rootObj["version"].toInt());
                }
            }
        }

        qint64 current_time_ms = QDateTime::currentMSecsSinceEpoch();

        // Iterate until the listening window has passed
        if ((current_time_ms - start_time_ms) > window.count())
            break;
    }
    fprintf(stderr, "CANblaster: stop listen\r\n");

    return detected_servers;
}
