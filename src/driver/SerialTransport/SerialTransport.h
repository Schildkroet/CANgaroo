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

#include <chrono>
#include <functional>
#include <memory>

#include <QByteArray>
#include <QString>

// Byte stream to a serial (or USB CDC) device, the part of QSerialPort a
// serial-port driver uses. Drivers talk to this instead of QSerialPort so
// tests can put a simulated device behind them (see tests/ulc_driver/).
//
// Blocking calls behave like their QSerialPort counterparts: they return as
// soon as the condition is met or the timeout has passed.
class SerialTransport
{
public:
    // The line is always 8N1 without flow control; only the rate varies.
    struct Settings
    {
        qint32 baudRate{115200};
    };

    virtual ~SerialTransport() = default;

    [[nodiscard]] virtual bool open(const QString &portName, const Settings &settings) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    // True once the port reported an error other than a timeout. An unplugged
    // USB adapter leaves the port open and only shows up here.
    [[nodiscard]] virtual bool hasFailed() const = 0;
    [[nodiscard]] virtual QString errorString() const = 0;

    // Discards data buffered in either direction.
    virtual void clear() = 0;

    // Returns the number of bytes accepted, -1 on error.
    virtual qint64 write(const QByteArray &data) = 0;
    virtual bool waitForBytesWritten(std::chrono::milliseconds timeout) = 0;

    [[nodiscard]] virtual qint64 bytesAvailable() = 0;
    virtual bool waitForReadyRead(std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual QByteArray readAll() = 0;
};

using SerialTransportFactory = std::function<std::unique_ptr<SerialTransport>()>;
