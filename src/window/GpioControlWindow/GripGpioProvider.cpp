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

#include "GripGpioProvider.h"

#include <utility>

#include "driver/GrIPDriver/GrIP/GrIPHandler.h"

GripGpioProvider::GripGpioProvider(GrIPHandler *handler, QString name, QObject *parent)
    : GpioProvider(parent)
    , _handler(handler)
    , _name(std::move(name))
{
    // gpioUpdated() crosses threads (GrIP serial thread → GUI) via a queued
    // connection, which copies the arguments — register the container type.
    qRegisterMetaType<QVector<uint16_t>>("QVector<uint16_t>");

    connect(_handler, &GrIPHandler::gpioUpdated, this,
            [this](uint16_t pinState, QVector<uint16_t> analogValues)
            {
                emit gpioUpdated(pinState, analogValues);
            });
}

void GripGpioProvider::setConfig(bool enable, uint8_t cycleMs, uint16_t dirMask)
{
    _handler->GpioSetConfig(enable, cycleMs, dirMask);
}

void GripGpioProvider::setOutput(uint16_t outputMask)
{
    _handler->GpioSetOutput(outputMask);
}
