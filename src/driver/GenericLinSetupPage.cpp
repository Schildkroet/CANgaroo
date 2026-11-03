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

#include "GenericLinSetupPage.h"
#include "ui_GenericLinSetupPage.h"
#include "LinFrameDefaultsDialog.h"
#include "core/Backend.h"

#include <QPushButton>
#include "db/model/LinDb.h"
#include "core/MeasurementInterface.h"
#include "core/MeasurementNetwork.h"
#include "driver/BusInterface.h"
#include "window/SetupDialog/SetupDialog.h"

GenericLinSetupPage::GenericLinSetupPage(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::GenericLinSetupPage),
      _mi(nullptr),
      _network(nullptr),
      _enableUiUpdates(false)
{
    ui->setupUi(this);

    populateBaudrates();
    populateProtocolVersions();

    connect(ui->cbBaudrate,        &QComboBox::currentIndexChanged, this, [this]() { updateUI(); });
    connect(ui->cbProtocolVersion, &QComboBox::currentIndexChanged, this, [this]() { updateUI(); });
    connect(ui->cbChecksumClassic, &QCheckBox::stateChanged, this, [this]() { updateUI(); });
    connect(ui->cbListenOnly,      &QCheckBox::stateChanged, this, [this]() { updateUI(); });

    connect(ui->cbLdfSelect,      &QComboBox::currentIndexChanged, this, &GenericLinSetupPage::onLdfSelected);
    connect(ui->cbScheduleTable,  &QComboBox::currentIndexChanged, this, [this]() { updateUI(); });
    connect(ui->rbMaster,         &QRadioButton::toggled,          this, [this]() { updateUI(); });
    connect(ui->rbSlave,          &QRadioButton::toggled,          this, [this](bool checked)
    {
        ui->cbSlaveNode->setEnabled(checked);
        updateUI();
    });
    connect(ui->cbSlaveNode,      &QComboBox::currentIndexChanged, this, [this]() { updateUI(); });
    connect(ui->pbFrameDefaults,  &QPushButton::clicked,           this, &GenericLinSetupPage::onConfigureFrameDefaults);
}

GenericLinSetupPage::~GenericLinSetupPage()
{
    delete ui;
}

void GenericLinSetupPage::onSetupDialogCreated(SetupDialog &dlg)
{
    dlg.addPage(this);
    connect(&dlg, &SetupDialog::onShowInterfacePage, this, &GenericLinSetupPage::onShowInterfacePage);
}

void GenericLinSetupPage::onShowInterfacePage(SetupDialog &dlg, MeasurementInterface *mi)
{
    if (mi->busType() != BusType::LIN)
        return;

    _mi      = mi;
    _network = dlg.currentNetwork();

    BusInterface *intf = backend().getInterfaceById(_mi->busInterface());
    _linCaps = intf->getCapabilities();
    ui->laDriver->setText(intf->getDriver()->getName());
    ui->laInterface->setText(QString("%1 - [ID: %2]").arg(intf->getName()).arg(intf->getId()));
    ui->laInterfaceDetails->setText(intf->getDetailsStr());

    _enableUiUpdates = false;

    // Baud rate
    populateBaudrates(intf);
    {
        // Keep a configured rate the interface does not list (e.g. from an older
        // config) instead of silently replacing it with the first entry.
        const unsigned baud = _mi->linBaudRate();
        int idx = ui->cbBaudrate->findData(baud);
        if (idx < 0 && baud > 0)
        {
            idx = 0;
            while (idx < ui->cbBaudrate->count() && ui->cbBaudrate->itemData(idx).toUInt() < baud)
                ++idx;
            ui->cbBaudrate->insertItem(idx, QString::number(baud), baud);
        }
        ui->cbBaudrate->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    // Protocol version
    ui->cbProtocolVersion->setCurrentIndex(static_cast<int>(_mi->linProtocolVersion()));

    ui->cbChecksumClassic->setChecked(_mi->linChecksumClassic());

    const bool capSlave = (_linCaps & BusInterface::capability_lin_slave) != 0;
    ui->cbListenOnly->setVisible(capSlave);
    ui->cbListenOnly->setChecked(capSlave && _mi->linListenOnly());

    // Populate and enable/disable the LDF group
    populateLdfCombo();

    _enableUiUpdates = true;
    dlg.displayPage(this);
}

void GenericLinSetupPage::updateUI()
{
    if (!_enableUiUpdates || !_mi)
        return;

    _mi->setLinChecksumClassic(ui->cbChecksumClassic->isChecked());

    const bool listenOnly = ui->cbListenOnly->isVisible() && ui->cbListenOnly->isChecked();
    _mi->setLinListenOnly(listenOnly);

    // LDF group
    if (ui->gbLdfConfig->isEnabled())
    {
        _mi->setLinBaudRate(ui->cbBaudrate->currentData().toUInt());
        _mi->setLinProtocolVersion(static_cast<LinProtocolVersion>(ui->cbProtocolVersion->currentIndex()));
        _mi->setLinScheduleTable(ui->cbScheduleTable->currentText());
        _mi->setLinScheduleTableIndex(static_cast<uint8_t>(ui->cbScheduleTable->currentIndex()));

        if (listenOnly)
        {
            ui->rbSlave->setChecked(true);
            ui->rbMaster->setEnabled(false);
            _mi->setLinNodeMode(LinNodeMode::Slave);
        }
        else
        {
            const bool capMaster = (_linCaps & BusInterface::capability_lin_master) != 0;
            ui->rbMaster->setEnabled(capMaster);
            _mi->setLinNodeMode(ui->rbMaster->isChecked() ? LinNodeMode::Master : LinNodeMode::Slave);
        }
        _mi->setLinSlaveNode(ui->rbSlave->isChecked() ? ui->cbSlaveNode->currentText() : QString());
    }
}

void GenericLinSetupPage::onLdfSelected(int index)
{
    updateLdfInfo(index);
    updateUI();
}

void GenericLinSetupPage::populateBaudrates(BusInterface *intf)
{
    // Rates the interface offers for LIN. LIN runs at up to 20 kbit/s, so a list
    // with faster rates is BusInterface's CAN default, not a LIN list.
    QList<unsigned> baudrates;
    if (intf)
    {
        for (const auto &timing : intf->getAvailableBitrates())
        {
            const unsigned br = timing.getBitrate();
            if (br > 20000)
            {
                baudrates.clear();
                break;
            }
            if (!baudrates.contains(br))
                baudrates.append(br);
        }
    }

    if (baudrates.isEmpty())
        baudrates = { 1000, 2400, 4800, 9600, 19200, 20000 };

    ui->cbBaudrate->clear();
    for (unsigned br : std::as_const(baudrates))
        ui->cbBaudrate->addItem(QString::number(br), br);
}

void GenericLinSetupPage::populateProtocolVersions()
{
    ui->cbProtocolVersion->clear();
    ui->cbProtocolVersion->addItem(QStringLiteral("LIN 1.3"),  static_cast<int>(LinProtocolVersion::V1_3));
    ui->cbProtocolVersion->addItem(QStringLiteral("LIN 2.0"),  static_cast<int>(LinProtocolVersion::V2_0));
    ui->cbProtocolVersion->addItem(QStringLiteral("LIN 2.1"),  static_cast<int>(LinProtocolVersion::V2_1));
    ui->cbProtocolVersion->addItem(QStringLiteral("LIN 2.2"),  static_cast<int>(LinProtocolVersion::V2_2));
    ui->cbProtocolVersion->addItem(QStringLiteral("LIN 2.2A"), static_cast<int>(LinProtocolVersion::V2_2A));
}

void GenericLinSetupPage::populateLdfCombo()
{
    const bool hasLdfs = _network && !_network->_linDbs.isEmpty();
    ui->gbLdfConfig->setEnabled(hasLdfs);

    _enableUiUpdates = false;

    ui->cbLdfSelect->clear();
    ui->cbScheduleTable->clear();
    ui->cbSlaveNode->clear();
    ui->cbSlaveNode->setEnabled(false);
    ui->laLdfTimebase->setText(QStringLiteral("-"));
    ui->laLdfJitter->setText(QStringLiteral("-"));

    if (!hasLdfs)
    {
        _enableUiUpdates = true;
        return;
    }

    // Populate LDF dropdown
    int selectIdx = 0;
    for (int i = 0; i < _network->_linDbs.size(); ++i)
    {
        const auto &ldb = _network->_linDbs.at(i);
        ui->cbLdfSelect->addItem(ldb->fileName());
        if (ldb->path() == _mi->linLdfPath())
            selectIdx = i;
    }
    ui->cbLdfSelect->setCurrentIndex(selectIdx);

    _enableUiUpdates = true;

    // Trigger info update for the selected LDF
    updateLdfInfo(selectIdx);

    // Restore node mode radio buttons and slave node enabled state.
    // If the device only supports one mode, force it and disable the other radio.
    const bool capMaster = (_linCaps & BusInterface::capability_lin_master) != 0;
    const bool capSlave  = (_linCaps & BusInterface::capability_lin_slave)  != 0;
    const bool forceMaster = capMaster && !capSlave;
    const bool forceSlave  = capSlave  && !capMaster;

    if (forceMaster)
    {
        ui->rbMaster->setChecked(true);
        ui->rbSlave->setChecked(false);
        _mi->setLinNodeMode(LinNodeMode::Master);
    }
    else if (forceSlave)
    {
        ui->rbSlave->setChecked(true);
        ui->rbMaster->setChecked(false);
        _mi->setLinNodeMode(LinNodeMode::Slave);
    }
    else
    {
        const bool isSlave = _mi->linNodeMode() == LinNodeMode::Slave;
        ui->rbMaster->setChecked(!isSlave);
        ui->rbSlave->setChecked(isSlave);
    }

    ui->rbMaster->setEnabled(!forceSlave);
    ui->rbSlave->setEnabled(!forceMaster);
    ui->cbSlaveNode->setEnabled(ui->rbSlave->isChecked());
}

void GenericLinSetupPage::updateLdfInfo(int ldfIndex)
{
    if (!_network || ldfIndex < 0 || ldfIndex >= _network->_linDbs.size())
    {
        ui->laLdfTimebase->setText(QStringLiteral("-"));
        ui->laLdfJitter->setText(QStringLiteral("-"));
        ui->cbScheduleTable->clear();
        ui->cbSlaveNode->clear();
        ui->cbSlaveNode->setEnabled(false);
        if (_mi)
        {
            _mi->setLinLdfPath(QString());
            _mi->setLinTimebaseMs(0);
            _mi->setLinJitterUs(0);
        }
        return;
    }

    const auto &ldb = _network->_linDbs.at(ldfIndex);

    ui->laLdfTimebase->setText(QString::number(ldb->masterTimebaseMs()) + QStringLiteral(" ms"));
    ui->laLdfJitter->setText(QString::number(ldb->masterJitterMs()) + QStringLiteral(" ms"));

    // Persist LDF-derived values to MeasurementInterface immediately
    if (_mi)
    {
        _mi->setLinLdfPath(ldb->path());
        _mi->setLinTimebaseMs(static_cast<uint8_t>(qBound(0.0, ldb->masterTimebaseMs(), 255.0)));
        _mi->setLinJitterUs(static_cast<uint16_t>(qBound(0.0, ldb->masterJitterMs() * 1000.0, 65535.0)));
    }

    // Sync combos and _mi with LDF values — suppress signals to avoid
    // updateUI() reading a mix of old and new combo states mid-update.
    const bool wasUpdating = _enableUiUpdates;
    _enableUiUpdates = false;

    int pvIdx = ui->cbProtocolVersion->findText(QStringLiteral("LIN ") + ldb->protocolVersion());
    if (pvIdx >= 0)
        ui->cbProtocolVersion->setCurrentIndex(pvIdx);

    const unsigned ldfBaud = static_cast<unsigned>(ldb->speedBps());
    int brIdx = ui->cbBaudrate->findData(ldfBaud);
    if (brIdx < 0)
    {
        ui->cbBaudrate->addItem(QString::number(ldfBaud), ldfBaud);
        brIdx = ui->cbBaudrate->count() - 1;
    }
    ui->cbBaudrate->setCurrentIndex(brIdx);

    // Write LDF-derived baud/protocol directly so _mi is always consistent.
    if (_mi)
    {
        _mi->setLinBaudRate(ldfBaud);
        if (pvIdx >= 0)
            _mi->setLinProtocolVersion(static_cast<LinProtocolVersion>(pvIdx));
    }

    // Populate schedule table and slave node dropdowns

    ui->cbScheduleTable->clear();
    for (const QString &name : ldb->scheduleTableNames())
        ui->cbScheduleTable->addItem(name);

    int tblIdx = ui->cbScheduleTable->findText(_mi->linScheduleTable());
    ui->cbScheduleTable->setCurrentIndex(tblIdx >= 0 ? tblIdx : 0);

    ui->cbSlaveNode->clear();
    for (const QString &name : ldb->slaveNodes())
        ui->cbSlaveNode->addItem(name);

    int slaveIdx = ui->cbSlaveNode->findText(_mi->linSlaveNode());
    ui->cbSlaveNode->setCurrentIndex(slaveIdx >= 0 ? slaveIdx : 0);

    _enableUiUpdates = wasUpdating;
}

void GenericLinSetupPage::onConfigureFrameDefaults()
{
    if (!_mi || !_network)
        return;

    const int ldfIdx = ui->cbLdfSelect->currentIndex();
    if (ldfIdx < 0 || ldfIdx >= _network->_linDbs.size())
        return;

    const auto &ldb = _network->_linDbs.at(ldfIdx);

    QString nodeName;
    if (ui->rbMaster->isChecked())
        nodeName = ldb->masterNode();
    else if (ui->rbSlave->isChecked())
        nodeName = ui->cbSlaveNode->currentText();

    LinFrameDefaultsDialog dlg(ldb.get(), nodeName, _mi->linFrameDefaultsRef(), this);
    dlg.exec();
}

Backend &GenericLinSetupPage::backend()
{
    return Backend::instance();
}
