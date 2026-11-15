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

// A simulated serial device and the port a driver reaches it through.
//
// FakeSerialPort behaves like QSerialPort on a thread without an event loop,
// which is where drivers read (BusListener). Bytes reach the port's buffer
// only inside waitForBytesWritten() and waitForReadyRead(), never in
// bytesAvailable() or readAll():
//   - waitForBytesWritten() also reads what the device has already sent.
//   - waitForReadyRead() returns true only for bytes that arrived during the
//     call, even if older ones are still buffered (QIODevice semantics).
// A driver that waits before looking at bytesAvailable() can therefore leave
// data in the buffer that it never reads.
//
// Each wait moves at most one full-speed USB packet (64 bytes) into the buffer,
// so a reply longer than that needs several waits, as on a slow host.
//
// The device's firmware main loop runs only inside waitForReadyRead(), the one
// place where the host gives the device time. Commands written back to back
// without such a wait reach the device together, as one USB transfer would.

#include <QByteArray>

#include "driver/SerialTransport/SerialTransport.h"

class FakeSerialPort;

class FakeSerialDevice
{
public:
    virtual ~FakeSerialDevice() = default;

    // Each call makes a port for one driver open(), all reaching this device.
    // The device must outlive every port it made.
    [[nodiscard]] SerialTransportFactory factory();

    void setPortOpenFails(bool fails) { _portOpenFails = fails; }
    // USB unplugged: the port stays open but reports an error, and every wait
    // returns false at once, as QSerialPort does after a ResourceError.
    void unplug() { _unplugged = true; }

protected:
    // Bytes the host wrote, as they arrive at the device.
    virtual void hostWrite(const QByteArray &data) = 0;
    // One pass of the firmware's main loop; may append to _output.
    virtual void runMainLoop() = 0;

    // Sent by the device, not yet read into the host port's buffer
    QByteArray _output;

private:
    friend class FakeSerialPort;

    bool _portOpenFails{false};
    bool _unplugged{false};
};

class FakeSerialPort final : public SerialTransport
{
public:
    explicit FakeSerialPort(FakeSerialDevice &device) : _dev(device) {}

    bool open(const QString &portName, const Settings &settings) override;
    void close() override;
    bool isOpen() const override;
    bool hasFailed() const override;
    QString errorString() const override;
    void clear() override;
    qint64 write(const QByteArray &data) override;
    bool waitForBytesWritten(std::chrono::milliseconds timeout) override;
    qint64 bytesAvailable() override;
    bool waitForReadyRead(std::chrono::milliseconds timeout) override;
    QByteArray readAll() override;

private:
    [[nodiscard]] bool usable() const;
    // Moves the next USB packet the device sent into the buffer; true if there was one.
    bool receive();

    FakeSerialDevice &_dev;
    QByteArray _buffer;
    bool _open{false};
};
