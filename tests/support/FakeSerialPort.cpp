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

#include "FakeSerialPort.h"

#include <thread>

SerialTransportFactory FakeSerialDevice::factory()
{
    return [this] { return std::make_unique<FakeSerialPort>(*this); };
}

bool FakeSerialPort::open(const QString &, const Settings &)
{
    _open = !_dev._portOpenFails;
    return _open;
}

void FakeSerialPort::close()
{
    _open = false;
    _buffer.clear();
}

bool FakeSerialPort::isOpen() const
{
    return _open;
}

bool FakeSerialPort::hasFailed() const
{
    return _dev._unplugged;
}

QString FakeSerialPort::errorString() const
{
    if (_dev._unplugged)
        return QStringLiteral("device unplugged");
    return _dev._portOpenFails ? QStringLiteral("cannot open port") : QString();
}

bool FakeSerialPort::usable() const
{
    return _open && !_dev._unplugged;
}

bool FakeSerialPort::receive()
{
    if (_dev._output.isEmpty())
        return false;
    _buffer += _dev._output;
    _dev._output.clear();
    return true;
}

// QSerialPort::clear(): drops the buffer and what is still in flight
void FakeSerialPort::clear()
{
    _buffer.clear();
    _dev._output.clear();
}

qint64 FakeSerialPort::write(const QByteArray &data)
{
    if (!usable())
        return -1;
    _dev.hostWrite(data);
    return data.size();
}

bool FakeSerialPort::waitForBytesWritten(std::chrono::milliseconds)
{
    if (!usable())
        return false;
    receive();  // reads while it waits to write
    return true;
}

qint64 FakeSerialPort::bytesAvailable()
{
    return _buffer.size();
}

bool FakeSerialPort::waitForReadyRead(std::chrono::milliseconds timeout)
{
    if (!usable())
        return false;

    _dev.runMainLoop();
    if (receive())
        return true;

    // Block like a real port, so driver timeouts behave as on hardware
    std::this_thread::sleep_for(timeout);
    _dev.runMainLoop();
    return receive();
}

QByteArray FakeSerialPort::readAll()
{
    QByteArray out;
    out.swap(_buffer);
    return out;
}
