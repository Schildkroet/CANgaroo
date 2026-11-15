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

#include "SerialTransport.h"

#include <QSerialPort>

// SerialTransport on a real serial port.
class QtSerialTransport final : public SerialTransport
{
public:
    [[nodiscard]] static SerialTransportFactory factory();

    [[nodiscard]] bool open(const QString &portName, const Settings &settings) override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;
    [[nodiscard]] bool hasFailed() const override;
    [[nodiscard]] QString errorString() const override;
    void clear() override;
    qint64 write(const QByteArray &data) override;
    bool waitForBytesWritten(std::chrono::milliseconds timeout) override;
    [[nodiscard]] qint64 bytesAvailable() override;
    bool waitForReadyRead(std::chrono::milliseconds timeout) override;
    [[nodiscard]] QByteArray readAll() override;

private:
    QSerialPort _port;
};
