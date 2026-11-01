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

#pragma once

#include <QString>
#include "core/Backend.h"
#include "driver/CanDriver.h"
#include "UlcInterface.h"

class GenericLinSetupPage;

class UlcDriver : public CanDriver
{
public:
    explicit UlcDriver(Backend &backend);
    ~UlcDriver() override;

    QString getName() const override;
    bool update() override;

private:
    UlcInterface *createOrUpdateInterface(int index, QString portName);
    GenericLinSetupPage *_setupPage;
};
