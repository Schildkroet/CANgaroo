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

#include "GpioControlWindow.h"

#include <algorithm>

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "core/Backend.h"
#include "core/MeasurementInterface.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementSetup.h"
#include "driver/BusInterface.h"
#include "driver/GpioProvider.h"
#include "driver/GrIPDriver/GrIPInterface.h"
#include "driver/AiodeDriver/AiodeApi.hpp"
#include "GripGpioProvider.h"

GpioControlWindow::GpioControlWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(backend)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(4, 4, 4, 4);

    // --- Top bar: manual rescan for hot-plugged aiode devices ---
    auto *topBar = new QWidget(this);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(0, 0, 0, 0);
    auto *rescanBtn = new QPushButton(tr("Rescan devices"), topBar);
    topLayout->addWidget(rescanBtn);
    topLayout->addStretch();
    outerLayout->addWidget(topBar);

    // One tab per device.
    _tabs = new QTabWidget(this);
    _tabs->setDocumentMode(true);
    outerLayout->addWidget(_tabs);

    // Shown instead of the (empty) tab widget when no devices are present.
    _placeholder = new QLabel(tr("No GPIO devices found"), this);
    _placeholder->setAlignment(Qt::AlignCenter);
    _placeholder->setEnabled(false);
    outerLayout->addWidget(_placeholder);

    connect(rescanBtn, &QPushButton::clicked, this, &GpioControlWindow::rebuildAll);
    connect(&_backend, &Backend::beginMeasurement, this, &GpioControlWindow::refreshGripPanels);
    connect(&_backend, &Backend::endMeasurement,   this, &GpioControlWindow::refreshGripPanels);

    // aiode devices are discovered independently of any measurement.
    rebuildAll();
}

GpioControlWindow::~GpioControlWindow()
{
    clearAllPanels();
}

void GpioControlWindow::retranslateUi()
{
    _placeholder->setText(tr("No GPIO devices found"));
}

/* -------------------------------------------------------------------------
 * Panel lifecycle
 * ------------------------------------------------------------------------- */

void GpioControlWindow::rebuildAll()
{
    clearAllPanels();
    addAiodePanels();
    addGripPanels();
    updatePlaceholder();
}

void GpioControlWindow::refreshGripPanels()
{
    removePanels(GpioDevicePanel::Source::Grip);
    addGripPanels();
    updatePlaceholder();
}

void GpioControlWindow::addGripPanels()
{
    if (!_backend.isMeasurementRunning())
        return;

    QSet<GrIPHandler *> seen;
    int deviceIndex = 0;

    for (MeasurementNetwork *network : _backend.getSetup().getNetworks())
    {
        for (MeasurementInterface *mi : network->interfaces())
        {
            BusInterface *iface = _backend.getInterfaceById(mi->busInterface());
            if (!iface)
                continue;

            auto *grip = qobject_cast<GrIPInterface *>(iface);
            if (!grip)
                continue;

            GrIPHandler *h = grip->handler();
            if (!h || seen.contains(h))
                continue;

            seen.insert(h);
            auto *provider = new GripGpioProvider(h, tr("GrIP Device %1").arg(++deviceIndex), this);
            buildDevicePanel(provider, GpioDevicePanel::Source::Grip, provider->name());
        }
    }
}

void GpioControlWindow::addAiodePanels()
{
    const QList<AiodeApi *> devices = AiodeApi::scan(this);
    for (AiodeApi *api : devices)
        buildDevicePanel(api, GpioDevicePanel::Source::Aiode, api->name());
}

void GpioControlWindow::removePanels(GpioDevicePanel::Source source)
{
    const auto panels = _panels; // copy: we mutate _panels in the loop
    for (auto it = panels.cbegin(); it != panels.cend(); ++it)
    {
        GpioDevicePanel *panel = it.value();
        if (panel->source != source)
            continue;

        if (panel->enabled)
            panel->provider->setConfig(false, static_cast<uint8_t>(panel->cycleSpin->value()), panel->dirMask);

        _panels.remove(it.key());
        const int tabIdx = _tabs->indexOf(panel->container);
        if (tabIdx >= 0)
            _tabs->removeTab(tabIdx);
        panel->container->deleteLater();
        delete panel->provider; // closes USB / joins poll thread (aiode) or disconnects (GrIP)
        delete panel;
    }
}

void GpioControlWindow::clearAllPanels()
{
    removePanels(GpioDevicePanel::Source::Grip);
    removePanels(GpioDevicePanel::Source::Aiode);
}

void GpioControlWindow::updatePlaceholder()
{
    const bool empty = _panels.isEmpty();
    _placeholder->setVisible(empty);
    _tabs->setVisible(!empty);
}

/* -------------------------------------------------------------------------
 * Panel construction
 * ------------------------------------------------------------------------- */

void GpioControlWindow::buildDevicePanel(GpioProvider *provider, GpioDevicePanel::Source source,
                                         const QString &deviceName)
{
    const int pinCount    = std::min(16, std::max(0, provider->digitalPinCount()));
    const int analogCount = provider->analogPinCount();

    auto *panel = new GpioDevicePanel{};
    panel->provider = provider;
    panel->source   = source;

    // Each device lives on its own scrollable tab page.
    auto *page = new QWidget;
    auto *outerLayout = new QVBoxLayout(page);
    outerLayout->setContentsMargins(6, 6, 6, 6);
    outerLayout->setSpacing(6);

    // --- Config bar ---
    auto *cfgBar = new QWidget(page);
    auto *cfgLayout = new QHBoxLayout(cfgBar);
    cfgLayout->setContentsMargins(0, 0, 0, 0);
    cfgLayout->setSpacing(6);

    auto *cycleLabel = new QLabel(tr("Update interval (ms):"), cfgBar);

    panel->cycleSpin = new QSpinBox(cfgBar);
    panel->cycleSpin->setRange(5, 500);
    panel->cycleSpin->setValue(50);
    panel->cycleSpin->setToolTip(tr("How often the device reports GPIO state (minimum 5 ms)"));

    panel->toggleBtn = new QPushButton(tr("Enable"), cfgBar);

    cfgLayout->addWidget(cycleLabel);
    cfgLayout->addWidget(panel->cycleSpin);
    cfgLayout->addWidget(panel->toggleBtn);
    cfgLayout->addStretch();

    outerLayout->addWidget(cfgBar);

    // --- Pin grid ---
    auto *gridWidget = new QWidget(page);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    // Header row
    int col = 0;
    auto makeHdr = [&](const QString &text)
    {
        auto *lbl = new QLabel(text, gridWidget);
        lbl->setStyleSheet("font-weight: bold;");
        grid->addWidget(lbl, 0, col++);
    };
    const QString analogUnit = provider->analogUnit();
    makeHdr(tr("Pin"));
    makeHdr(tr("Direction"));
    makeHdr(tr("Digital"));
    makeHdr(analogUnit.isEmpty() ? tr("Analog") : tr("Voltage (%1)").arg(analogUnit));
    makeHdr(tr("Output"));

    // Pin rows
    for (int pin = 0; pin < pinCount; ++pin)
    {
        const int row = pin + 1;

        auto *pinLbl = new QLabel(QString::number(pin), gridWidget);
        pinLbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(pinLbl, row, 0);

        auto *dirCombo = new QComboBox(gridWidget);
        dirCombo->addItem(tr("Input"));
        dirCombo->addItem(tr("Output"));
        grid->addWidget(dirCombo, row, 1);

        auto *digitalLbl = new QLabel(QStringLiteral("--"), gridWidget);
        digitalLbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(digitalLbl, row, 2);

        auto *voltLbl = new QLabel(pin < analogCount ? QStringLiteral("--") : tr("N/A"), gridWidget);
        voltLbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(voltLbl, row, 3);

        auto *outputBtn = new QPushButton(tr("Set HIGH"), gridWidget);
        outputBtn->setVisible(false);
        grid->addWidget(outputBtn, row, 4);

        GpioPinRow pinRow{pin, dirCombo, digitalLbl, voltLbl, outputBtn};
        panel->pinRows.append(pinRow);

        connect(dirCombo, &QComboBox::currentIndexChanged, this,
                [panel, pin](int index)
                {
                    if (index == 1)
                        panel->dirMask |= static_cast<uint16_t>(1u << pin);
                    else
                        panel->dirMask &= static_cast<uint16_t>(~(1u << pin));
                    panel->pinRows[pin].outputBtn->setVisible(index == 1);
                });

        connect(outputBtn, &QPushButton::clicked, this,
                [this, panel, pin]() { onOutputToggled(panel, pin); });
    }

    outerLayout->addWidget(gridWidget);
    outerLayout->addStretch(); // keep the grid top-aligned within the tab

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);

    panel->container = scroll;
    _tabs->addTab(scroll, deviceName);

    connect(panel->toggleBtn, &QPushButton::clicked, this,
            [this, panel]() { onToggleClicked(panel); });

    // AiodeApi emits gpioUpdated from a bare std::thread, so force a queued
    // delivery onto the GUI thread rather than relying on auto-connection
    // inferring it from the (foreign) emitter thread.
    connect(provider, &GpioProvider::gpioUpdated, this,
            [this, panel](uint16_t pinState, QVector<uint16_t> analogValues)
            {
                onGpioUpdated(panel, pinState, analogValues);
            },
            Qt::QueuedConnection);

    _panels.insert(provider, panel);
}

/* -------------------------------------------------------------------------
 * Interaction
 * ------------------------------------------------------------------------- */

void GpioControlWindow::onToggleClicked(GpioDevicePanel *panel)
{
    panel->enabled = !panel->enabled;

    if (panel->enabled)
    {
        uint16_t dirMask = 0;
        for (const GpioPinRow &row : std::as_const(panel->pinRows))
        {
            if (row.dirCombo->currentIndex() == 1)
                dirMask |= static_cast<uint16_t>(1u << row.pin);
        }
        panel->dirMask = dirMask;
    }

    for (const GpioPinRow &row : std::as_const(panel->pinRows))
        row.dirCombo->setEnabled(!panel->enabled);

    panel->cycleSpin->setEnabled(!panel->enabled);
    panel->toggleBtn->setText(panel->enabled ? tr("Disable") : tr("Enable"));

    panel->provider->setConfig(
        panel->enabled,
        static_cast<uint8_t>(panel->cycleSpin->value()),
        panel->dirMask);
}

void GpioControlWindow::onOutputToggled(GpioDevicePanel *panel, int pin)
{
    panel->outputMask ^= static_cast<uint16_t>(1u << pin);

    const bool isHigh = (panel->outputMask >> pin) & 1u;
    panel->pinRows[pin].outputBtn->setText(isHigh ? tr("Set LOW") : tr("Set HIGH"));

    GpioPinRow &row = panel->pinRows[pin];
    row.digitalLbl->setText(isHigh ? tr("HIGH") : tr("LOW"));
    row.digitalLbl->setStyleSheet(isHigh
        ? QStringLiteral("color: #00cc00; font-weight: bold;")
        : QStringLiteral("color: #cc0000; font-weight: bold;"));

    panel->provider->setOutput(panel->outputMask);
}

void GpioControlWindow::onGpioUpdated(GpioDevicePanel *panel, uint16_t pinState,
                                       const QVector<uint16_t> &analogValues)
{
    const int analogCount = panel->provider->analogPinCount();
    const QString analogUnit = panel->provider->analogUnit();

    for (GpioPinRow &row : panel->pinRows)
    {
        const bool isOutput = (panel->dirMask >> row.pin) & 1u;
        const bool high     = isOutput ? ((panel->outputMask >> row.pin) & 1u)
                                       : ((pinState >> row.pin) & 1u);

        row.digitalLbl->setText(high ? tr("HIGH") : tr("LOW"));
        row.digitalLbl->setStyleSheet(high
            ? QStringLiteral("color: #00cc00; font-weight: bold;")
            : QStringLiteral("color: #cc0000; font-weight: bold;"));

        if (row.pin < analogCount && row.pin < analogValues.size())
            row.voltLbl->setText(analogUnit.isEmpty()
                ? QString::number(analogValues[row.pin])
                : QString::number(analogValues[row.pin]) + QStringLiteral(" ") + analogUnit);

        row.outputBtn->setVisible(isOutput);
    }
}
