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

#include "driver/GpioProvider.h"

class GrIPHandler;

// Adapts an existing GrIPHandler to the GpioProvider interface so the GPIO
// Control window can drive GrIP and aiode devices through one code path.
class GripGpioProvider : public GpioProvider
{
    Q_OBJECT

public:
    GripGpioProvider(GrIPHandler *handler, QString name, QObject *parent = nullptr);

    QString name() const override { return _name; }
    int  digitalPinCount() const override { return 16; }
    int  analogPinCount() const override { return 8; }
    QString analogUnit() const override { return QStringLiteral("mV"); }
    // The GrIP GPIO config carries the cycle time in one byte.
    int maxCycleMs() const override { return 255; }
    void setConfig(bool enable, uint16_t cycleMs, uint16_t dirMask) override;
    void setOutput(uint16_t outputMask) override;

private:
    GrIPHandler *_handler;
    QString      _name;
};
