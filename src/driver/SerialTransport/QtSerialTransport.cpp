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

#include "QtSerialTransport.h"

SerialTransportFactory QtSerialTransport::factory()
{
    return [] { return std::make_unique<QtSerialTransport>(); };
}

bool QtSerialTransport::open(const QString &portName, const Settings &settings)
{
    _port.setPortName(portName);
    _port.setBaudRate(settings.baudRate);
    _port.setDataBits(QSerialPort::Data8);
    _port.setParity(QSerialPort::NoParity);
    _port.setStopBits(QSerialPort::OneStop);
    _port.setFlowControl(QSerialPort::NoFlowControl);
    return _port.open(QIODevice::ReadWrite);
}

void QtSerialTransport::close()
{
    _port.close();
}

bool QtSerialTransport::isOpen() const
{
    return _port.isOpen();
}

bool QtSerialTransport::hasFailed() const
{
    const QSerialPort::SerialPortError err = _port.error();
    return err != QSerialPort::NoError && err != QSerialPort::TimeoutError;
}

QString QtSerialTransport::errorString() const
{
    return _port.errorString();
}

void QtSerialTransport::clear()
{
    _port.clear();
}

qint64 QtSerialTransport::write(const QByteArray &data)
{
    return _port.write(data);
}

bool QtSerialTransport::waitForBytesWritten(std::chrono::milliseconds timeout)
{
    return _port.waitForBytesWritten(static_cast<int>(timeout.count()));
}

qint64 QtSerialTransport::bytesAvailable()
{
    return _port.bytesAvailable();
}

bool QtSerialTransport::waitForReadyRead(std::chrono::milliseconds timeout)
{
    return _port.waitForReadyRead(static_cast<int>(timeout.count()));
}

QByteArray QtSerialTransport::readAll()
{
    return _port.readAll();
}
