/*

  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

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

#include <cstdint>

#include <QObject>
#include <QString>
#include <QVector>

// Transport-agnostic interface for a device that exposes digital I/O lines
// (and optionally analog channels) to the GPIO Control window. Both the
// serial GrIP adapter and the USB aiode adapter implement this so the window
// can drive either kind of device through the same code path.
class GpioProvider : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~GpioProvider() override = default;

    /// Human-readable label shown as the panel title.
    virtual QString name() const = 0;

    /// Number of digital I/O lines to expose as rows (clamped to 16 by the UI).
    virtual int digitalPinCount() const = 0;

    /// Lines 0..analogPinCount()-1 also report an analog value; others show N/A.
    virtual int analogPinCount() const = 0;

    /// Unit suffix for analog values (e.g. "mV"); empty for raw counts.
    virtual QString analogUnit() const { return {}; }

    /// Longest report interval in ms the device can be configured for.
    virtual int maxCycleMs() const { return 500; }

    /// Enable/disable reporting. @p cycleMs is the report interval in ms
    /// (5..maxCycleMs()), @p dirMask sets per-line direction (bit set = output).
    virtual void setConfig(bool enable, uint16_t cycleMs, uint16_t dirMask) = 0;

    /// Drive the output lines to the levels in @p outputMask (bit set = HIGH).
    virtual void setOutput(uint16_t outputMask) = 0;

signals:
    /// Emitted whenever a fresh state report arrives. @p pinState is the digital
    /// level bitmask; @p analogValues holds one entry per analog channel.
    void gpioUpdated(uint16_t pinState, QVector<uint16_t> analogValues);
};
