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

#include "UsbVendorInterface.h"

#include <QMutexLocker>

// Platform-independent parts; transfers live in UsbVendorInterfaceLibusb.cpp
// (Linux/macOS) and UsbVendorInterfaceWinUsb.cpp (Windows).

UsbVendorInterface::Status UsbVendorInterface::fail(Status status, const std::string &context,
                                                    const std::string &reason)
{
    QMutexLocker lock(&_errorMutex);
    _lastError = context + ": " + reason;
    return status;
}

std::string UsbVendorInterface::lastError() const
{
    QMutexLocker lock(&_errorMutex);
    return _lastError;
}
