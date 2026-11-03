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

#pragma once

#include <QWidget>

namespace Ui {
class GenericLinSetupPage;
}

class BusInterface;
class SetupDialog;
class MeasurementInterface;
class MeasurementNetwork;
class Backend;

class GenericLinSetupPage : public QWidget
{
    Q_OBJECT

public:
    explicit GenericLinSetupPage(QWidget *parent = nullptr);
    ~GenericLinSetupPage();

public slots:
    void onSetupDialogCreated(SetupDialog &dlg);
    void onShowInterfacePage(SetupDialog &dlg, MeasurementInterface *mi);

private slots:
    void updateUI();
    void onLdfSelected(int index);
    void onConfigureFrameDefaults();

private:
    Ui::GenericLinSetupPage *ui;
    MeasurementInterface    *_mi;
    MeasurementNetwork      *_network;
    bool                     _enableUiUpdates;
    uint32_t                 _linCaps{0};

    void populateBaudrates(BusInterface *intf = nullptr);
    void populateProtocolVersions();
    void populateLdfCombo();
    void updateLdfInfo(int ldfIndex);

    Backend &backend();
};
