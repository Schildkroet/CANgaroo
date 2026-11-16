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

#include "FakeCanblasterServer.h"

#include <algorithm>

#include <QNetworkDatagram>
#include <QVariant>

FakeCanblasterServer::FakeCanblasterServer(std::chrono::milliseconds clientTimeout)
    : _clientTimeout(clientTimeout)
{
    // bind_ip default '0.0.0.0', port 20002
    _ok = _server.bind(QHostAddress::AnyIPv4, k_port);
}

void FakeCanblasterServer::beacon(QUdpSocket &socket)
{
    // IP_MULTICAST_TTL 1 (bind_ttl default); the literal message of
    // send_beacon(), including its space
    socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    socket.writeDatagram(QByteArray(R"({"protocol":"CANblaster", "version":1})"),
                             QHostAddress(QStringLiteral("239.255.43.21")), k_beaconPort);
}

void FakeCanblasterServer::takeHeartbeats()
{
    while (_server.hasPendingDatagrams())
    {
        const QNetworkDatagram d = _server.receiveDatagram(1024);
        auto it = std::find_if(_clients.begin(), _clients.end(), [&](const Client &c) {
            return c.address == d.senderAddress() && c.port == d.senderPort();
        });
        if (it == _clients.end())
        {
            _clients.append(Client{d.senderAddress(), static_cast<quint16>(d.senderPort()), {}});
            it = std::prev(_clients.end());
        }
        it->lastHeartbeat = std::chrono::steady_clock::now();
    }
}

int FakeCanblasterServer::liveClients()
{
    takeHeartbeats();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<int>(std::count_if(_clients.cbegin(), _clients.cend(), [&](const Client &c) {
        return now - c.lastHeartbeat < _clientTimeout;
    }));
}

// struct can_frame (linux/can.h): can_id (host order, little endian here),
// len, __pad, __res0, len8_dlc, data[8]
QByteArray FakeCanblasterServer::canFrameBytes(const Frame &frame)
{
    QByteArray out(16, '\0');
    const uint32_t id = frame.id | (frame.extended ? 0x80000000u : 0) | (frame.rtr ? 0x40000000u : 0);
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<char>((id >> (8 * i)) & 0xFF);
    out[4] = static_cast<char>(frame.len);
    for (int i = 0; i < frame.data.size() && i < 8; ++i)
        out[8 + i] = frame.data.at(i);
    return out;
}

void FakeCanblasterServer::sendCanFrame(const Frame &frame)
{
    takeHeartbeats();
    const QByteArray bytes = canFrameBytes(frame);
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < _clients.size();)
    {
        if (now - _clients[i].lastHeartbeat < _clientTimeout)
        {
            _server.writeDatagram(bytes, _clients[i].address, _clients[i].port);
            ++i;
        }
        else
        {
            _clients.removeAt(i);  // "client disconnected"
        }
    }
}
