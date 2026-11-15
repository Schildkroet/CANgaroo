/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>
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
#include "CandleApiDriver.h"
#include "api/candle.h"

#include "core/Log.h"
#include "CandleApiInterface.h"
#include "driver/GenericCanSetupPage.h"

#include <QString>

CandleApiDriver::CandleApiDriver(Backend &backend)
  : CanDriver(backend),
    setupPage(new GenericCanSetupPage(0))
{
    candle_log_fn = nullptr;
    candle_log_verbose = false;
    QObject::connect(&backend, &Backend::onSetupDialogCreated, setupPage, &GenericCanSetupPage::onSetupDialogCreated);
}

QString CandleApiDriver::getName() const
{
    return "CandleAPI";
}

bool CandleApiDriver::update()
{
    // Destroy existing interfaces first, then release shared devices.
    // Interfaces hold shared_ptr refs to the devices, so deleting them first
    // ensures the devices are freed in the correct order.
    deleteAllInterfaces();
    _devices.clear();

    for (const CandleDiscovered &found : candleDiscoverDevices()) {
        _devices[found.baseKey] = found.device;

        // One BusInterface per channel, all sharing the same physical device.
        for (uint8_t ch = 0; ch < found.channels; ch++) {
            addInterface(new CandleApiInterface(this, found.device, ch, [this] {
                return backend().getUsecsAtMeasurementStart() + backend().getUsecsSinceMeasurementStart();
            }));
        }
    }
    return true;
}
