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

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>

// Windows reports the COM port driver's description to Qt; the device's own
// USB product string is kept as DEVPKEY_Device_BusReportedDeviceDesc. On a
// composite device the COM port node may only carry the interface's name, so
// the parent device is asked too.
static QString usbProductString(const QString &portName)
{
    QString result;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVINFO_DATA dev{.cbSize = sizeof(SP_DEVINFO_DATA)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i)
    {
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        wchar_t name[32]{};
        DWORD size = sizeof(name);
        const bool match = RegQueryValueExW(key, L"PortName", nullptr, nullptr,
                                            reinterpret_cast<BYTE *>(name), &size) == ERROR_SUCCESS
                           && QString::fromWCharArray(name) == portName;
        RegCloseKey(key);
        if (!match)
            continue;

        DEVPROPTYPE type{};
        wchar_t desc[256]{};
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_BusReportedDeviceDesc, &type,
                                      reinterpret_cast<BYTE *>(desc), sizeof(desc), nullptr, 0))
            result = QString::fromWCharArray(desc);

        DEVINST parent{};
        if (!result.contains("LinUSBConverter") && CM_Get_Parent(&parent, dev.DevInst, 0) == CR_SUCCESS)
        {
            wchar_t parentDesc[256]{};
            ULONG parentSize = sizeof(parentDesc);
            if (CM_Get_DevNode_PropertyW(parent, &DEVPKEY_Device_BusReportedDeviceDesc, &type,
                                         reinterpret_cast<BYTE *>(parentDesc), &parentSize, 0) == CR_SUCCESS)
                result = QString::fromWCharArray(parentDesc);
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}
#endif

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
        const uint16_t vid = info.vendorIdentifier();
        const uint16_t pid = info.productIdentifier();

        // Firmware 3.5 and later has its own ID from pid.codes.
        const bool ownId = vid == 0x1209 && pid == 0x2779;

        // Older firmware uses ST's virtual COM port VID/PID, shared with WeAct
        // USB2CAN (serial "AAA...", handled by the SLCAN driver) and other STM32
        // boards, so there the USB product string has to match as well.
        // UlcInterface::open() also checks the firmware's version reply.
        if (!ownId)
        {
            if (vid != 0x0483 || pid != 0x5740)
                continue;
            if (info.serialNumber().startsWith("AAA"))
                continue;
#ifdef Q_OS_WIN
            const QString product = usbProductString(info.portName());
#else
            const QString product = info.description();
#endif
            if (!product.contains("LinUSBConverter"))
                continue;
        }

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
