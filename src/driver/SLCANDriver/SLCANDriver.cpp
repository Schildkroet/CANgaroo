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

/*

  Copyright (c) 2022 Ethan Zonca

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

#include "SLCANDriver.h"
#include "SLCANInterface.h"
#include "core/Backend.h"
#include "driver/GenericCanSetupPage.h"
#include "driver/SerialTransport/QtSerialTransport.h"

#include <iostream>

#include <QtSerialPort/QSerialPortInfo>

SLCANDriver::SLCANDriver(Backend &backend)
    : CanDriver(backend)
    , setupPage(new GenericCanSetupPage())
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated,
                     setupPage, &GenericCanSetupPage::onSetupDialogCreated);
}

SLCANDriver::~SLCANDriver() = default;

QString SLCANDriver::getName() const
{
    return "SLCAN";
}

bool SLCANDriver::update()
{
    deleteAllInterfaces();

    using Mfr = SLCANInterface::Manufacturer;
    int ifaceIdx = 0;

    for (const auto &info : QSerialPortInfo::availablePorts())
    {
        const uint16_t vid = info.vendorIdentifier();
        const uint16_t pid = info.productIdentifier();

        if ((vid == 0xAD50 && pid == 0x60C4) ||
            (vid == 0x0403 && pid == 0x6015))
        {
            std::cout << "   ++ CANable 1.0 detected: " << info.portName().toStdString() << '\n';
            createOrUpdateInterface(ifaceIdx++, info.portName(), false, Mfr::CANable);
        }
        else if (vid == 0x16D0 && pid == 0x117E)
        {
            std::cout << "   ++ CANable 2.0 detected: " << info.portName().toStdString() << '\n';
            createOrUpdateInterface(ifaceIdx++, info.portName(), true, Mfr::CANable);
        }
        else if (vid == 0x0483 && pid == 0x5740 && info.serialNumber().startsWith("AAA"))
        {
            std::cout << "   ++ WeAct Studio USB2CAN detected: "
                      << info.portName().toStdString() << " (" << info.serialNumber().toStdString() << ")\n";
            createOrUpdateInterface(ifaceIdx++, info.portName(), true, Mfr::WeActStudio);
        }
    }

    return true;
}

SLCANInterface *SLCANDriver::createOrUpdateInterface(int index, QString name, bool fdSupport,
                                                     SLCANInterface::Manufacturer manufacturer)
{
    for (auto *intf : getInterfaces())
    {
        auto *scif = dynamic_cast<SLCANInterface *>(intf);
        if (scif && scif->getIfIndex() == index)
        {
            scif->setName(name);
            return scif;
        }
    }

    auto *scif = new SLCANInterface(this, index, std::move(name), fdSupport, manufacturer,
                                   QtSerialTransport::factory());
    addInterface(scif);
    return scif;
}
