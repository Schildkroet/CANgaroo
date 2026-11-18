/*

  Copyright (c) 2024 - 2026 Schildkroet

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

#include "GrIPDriver.h"
#include "GrIPInterface.h"
#include "core/Backend.h"
#include "driver/GenericCanSetupPage.h"
#include "driver/GenericLinSetupPage.h"

#include <unistd.h>
#include <iostream>

#include <QCoreApplication>
#include <QDebug>
#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>
#include <QThread>


GrIPDriver::GrIPDriver(Backend &backend)
    : CanDriver(backend),
      setupPage(new GenericCanSetupPage()),
      linSetupPage(new GenericLinSetupPage())
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated, setupPage,    &GenericCanSetupPage::onSetupDialogCreated);
    QObject::connect(&backend, &Backend::onSetupDialogCreated, linSetupPage, &GenericLinSetupPage::onSetupDialogCreated);
    m_GrIPHandler = nullptr;
}

GrIPDriver::~GrIPDriver()
{
    if (m_GrIPHandler)
        delete m_GrIPHandler;
}

bool GrIPDriver::update()
{
    deleteAllInterfaces();

    int interface_cnt = 0;

    for (const auto &info : QSerialPortInfo::availablePorts())
    {
        // fprintf(stderr, "Name : %s \r\n",  info.portName().toStdString().c_str());
        // fprintf(stderr, "   Description : %s \r\n", info.description().toStdString().c_str());
        // fprintf(stderr, "   Manufacturer: %s \r\n", info.manufacturer().toStdString().c_str());

        if (info.vendorIdentifier() == 0x1A86 && info.productIdentifier() == 0x55D3)
        {
            std::cout << "   ++ CANIL detected" << std::endl;

            if (m_GrIPHandler == nullptr)
            {
                m_GrIPHandler = new GrIPHandler(info.portName());

                if (!m_GrIPHandler->Start())
                {
                }

                m_GrIPHandler->RequestVersion();
                QThread::msleep(15);
            }

            for (int i = 0; i < m_GrIPHandler->Channels_CAN(); i++)
            {
                createOrUpdateInterface(interface_cnt, i, m_GrIPHandler, "CANIL-CAN" + QString::number(interface_cnt), false, GrIPInterface::CANIL_CAN);
                interface_cnt++;
            }
            for (int i = 0; i < m_GrIPHandler->Channels_CANFD(); i++)
            {
                createOrUpdateInterface(interface_cnt, i, m_GrIPHandler, "CANIL-CANFD" + QString::number(interface_cnt), true, GrIPInterface::CANIL_CAN);
                interface_cnt++;
            }
            for (int i = 0; i < m_GrIPHandler->Channels_LIN(); i++)
            {
                createOrUpdateInterface(interface_cnt, i, m_GrIPHandler, "CANIL-LIN" + QString::number(i), false, GrIPInterface::CANIL_LIN);
                interface_cnt++;
            }
        }
        else
        {
            // std::cout << "   !! This is not a GrIP device!" << std::endl;
        }
    }

    return true;
}

QString GrIPDriver::getName() const
{
    return "GrIP-CANIL";
}

GrIPInterface *GrIPDriver::createOrUpdateInterface(int index, int channel_idx, GrIPHandler *hdl, QString name, bool fd_support, uint32_t manufacturer)
{
    for (auto *intf : getInterfaces())
    {
        GrIPInterface *scif = dynamic_cast<GrIPInterface *>(intf);
        if (scif->getIfIndex() == index)
        {
            scif->setName(name);
            return scif;
        }
    }

    GrIPInterface *scif = new GrIPInterface(this, index, channel_idx, hdl, name, fd_support, manufacturer);
    scif->refreshCapabilities();
    addInterface(scif);

    return scif;
}
