/*

  Copyright (c) 2026 Lukasz Juranek

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

#include "UlcDriver.h"
#include "UlcInterface.h"
#include "core/Backend.h"
#include "driver/GenericLinSetupPage.h"

#include <iostream>

#include <QtSerialPort/QSerialPortInfo>

UlcDriver::UlcDriver(Backend &backend)
    : CanDriver(backend)
    , _setupPage(new GenericLinSetupPage())
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated,
                     _setupPage, &GenericLinSetupPage::onSetupDialogCreated);
}

UlcDriver::~UlcDriver() = default;

QString UlcDriver::getName() const
{
    return "uLC";
}

bool UlcDriver::update()
{
    deleteAllInterfaces();

    int ifaceIdx = 0;

    for (const auto &info : QSerialPortInfo::availablePorts())
    {
        // The uLC uses ST's virtual COM port VID/PID, shared with WeAct USB2CAN
        // (serial "AAA...", handled by the SLCAN driver) and other STM32 boards.
        if (info.vendorIdentifier() != 0x0483 || info.productIdentifier() != 0x5740)
            continue;
        if (info.serialNumber().startsWith("AAA"))
            continue;

        // Linux and macOS report the USB product string. Windows reports the
        // COM port driver's strings instead, so there any remaining port is a
        // candidate and UlcInterface::open() checks the firmware's version reply.
#ifndef Q_OS_WIN
        if (!info.description().contains("LinUSBConverter"))
            continue;
#endif

        std::cout << "   ++ uLC detected: " << info.portName().toStdString() << '\n';
        createOrUpdateInterface(ifaceIdx++, info.portName());
    }

    return true;
}

UlcInterface *UlcDriver::createOrUpdateInterface(int index, QString portName)
{
    for (auto *intf : getInterfaces())
    {
        auto *ulcif = dynamic_cast<UlcInterface *>(intf);
        if (ulcif && ulcif->getIfIndex() == index)
        {
            ulcif->setName(portName);
            return ulcif;
        }
    }

    auto *ulcif = new UlcInterface(this, index, std::move(portName));
    addInterface(ulcif);
    return ulcif;
}
