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

#pragma once

// A CANblaster server on real UDP sockets, transcribed from
// github.com/normaldotcom/canblaster canblaster.py (e213ece), not from cangaroo:
//   - beacon '{"protocol":"CANblaster", "version":1}' to 239.255.43.21:20000
//   - listens on UDP 20002; ANY datagram is a heartbeat, the client is its
//     source address and port, kept for client_timeout (default 5 s)
//   - every CAN frame read from SocketCAN (CAN_RAW, no FD, no error frames)
//     goes to each live client as the raw 16-byte struct can_frame, from the
//     server socket (source port 20002)
//   - nothing the clients send is put on the bus (TX is a TODO there)

#include <chrono>
#include <cstdint>

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QUdpSocket>

class FakeCanblasterServer
{
public:
    static constexpr quint16 k_port = 20002;
    static constexpr quint16 k_beaconPort = 20000;

    struct Frame
    {
        uint32_t id{0};
        bool extended{false};
        bool rtr{false};
        uint8_t len{0};
        QByteArray data;
    };

    explicit FakeCanblasterServer(std::chrono::milliseconds clientTimeout = std::chrono::seconds(5));

    [[nodiscard]] bool ok() const { return _ok; }
    // One beacon to the multicast group (send_beacon()), from `socket`
    static void beacon(QUdpSocket &socket);
    // A frame from the bus, forwarded to every live client. Heartbeats that
    // arrived meanwhile are taken first, as the server loop does.
    void sendCanFrame(const Frame &frame);
    // Number of clients a frame would go to now
    [[nodiscard]] int liveClients();

    static QByteArray canFrameBytes(const Frame &frame);

private:
    void takeHeartbeats();

    struct Client
    {
        QHostAddress address;
        quint16 port{0};
        std::chrono::steady_clock::time_point lastHeartbeat;
    };

    std::chrono::milliseconds _clientTimeout;
    QUdpSocket _server;
    QList<Client> _clients;
    bool _ok{false};
};
