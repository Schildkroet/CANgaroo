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


#include "CanStatusWindow.h"
#include "ui_CanStatusWindow.h"

#include <QStringList>
#include <QTimer>
#include <QDateTime>
#include "core/Backend.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"

CanStatusWindow::CanStatusWindow(QWidget *parent, Backend &backend) :
    ConfigurableWidget(parent),
    ui(new Ui::CanStatusWindow),
    _backend(backend),
    _timer(new QTimer(this)),
    _stopTimer(new QTimer(this))
{
    _stopTimer->setSingleShot(true);
    connect(_stopTimer, &QTimer::timeout, this, [this]()
    {
        for (QTreeWidgetItemIterator it(ui->treeWidget); *it; ++it)
        {
            (*it)->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<void*>(nullptr)));
        }
        _timer->stop();
    });

    ui->setupUi(this);
    ui->treeWidget->setHeaderLabels(QStringList()
                                    << tr("Driver") << tr("Interface") << tr("State")
                                    << tr("Rx Frames") << tr("Rx Errors") << tr("Rx Overrun")
                                    << tr("Tx Frames") << tr("Tx Errors") << tr("Tx Dropped")
                                    << tr("Load (%)") << tr("Bits")
        // << "# Warning" << "# Passive" << "# Bus Off" << " #Restarts"
    );
    // Driver width
    ui->treeWidget->setColumnWidth(0, 100);
    // Interface width
    ui->treeWidget->setColumnWidth(1, 110);
    // State width
    ui->treeWidget->setColumnWidth(2, 80);
    // Rx Frame width
    ui->treeWidget->setColumnWidth(3, 90);
    // Rx Errors width
    ui->treeWidget->setColumnWidth(4, 80);
    // Rx Overrun width
    ui->treeWidget->setColumnWidth(5, 90);
    // Tx Frame width
    ui->treeWidget->setColumnWidth(6, 90);
    // Tx Errors width
    ui->treeWidget->setColumnWidth(7, 90);
    // Tx Dropped width
    ui->treeWidget->setColumnWidth(8, 90);
    // Bus Load width
    ui->treeWidget->setColumnWidth(column_bus_load, 90);

    connect(&backend, &Backend::beginMeasurement, this, &CanStatusWindow::beginMeasurement);
    connect(&backend, &Backend::endMeasurement, this, &CanStatusWindow::endMeasurement);
    connect(&backend, &Backend::onClearTraceRequested, this, &CanStatusWindow::clearStatistics);
    connect(_timer, &QTimer::timeout, this, &CanStatusWindow::update);
}

CanStatusWindow::~CanStatusWindow()
{
    delete ui;
}

void CanStatusWindow::retranslateUi()
{
    ui->retranslateUi(this);

    ui->treeWidget->setHeaderLabels(QStringList()
                                    << tr("Driver") << tr("Interface") << tr("State")
                                    << tr("Rx Frames") << tr("Rx Errors") << tr("Rx Overrun")
                                    << tr("Tx Frames") << tr("Tx Errors") << tr("Tx Dropped")
                                    // << "# Warning" << "# Passive" << "# Bus Off" << " #Restarts"
                                    );
}

void CanStatusWindow::beginMeasurement()
{
    ui->treeWidget->clear();
    _lastStats.clear();
    _effectiveBitrates.clear();

    // Build effective bitrate map: use LIN baud rate for LIN interfaces.
    for (MeasurementNetwork *network : backend().getSetup().getNetworks()) {
        for (MeasurementInterface *mi : network->interfaces()) {
            BusInterface *intf = backend().getInterfaceById(mi->busInterface());
            if (!intf) continue;
            unsigned bitrate = (mi->busType() == BusType::LIN) ? mi->linBaudRate() : mi->bitrate();
            _effectiveBitrates[intf] = bitrate;
        }
    }

    for (auto ifid : backend().getInterfaceList()) {
        BusInterface *intf = backend().getInterfaceById(ifid);
        QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeWidget);
        item->setData(0, Qt::UserRole, QVariant::fromValue((void*)intf));
        item->setText(column_driver, intf->getDriver()->getName());
        item->setText(column_interface, intf->getName());

        item->setTextAlignment(column_driver, Qt::AlignLeft);
        item->setTextAlignment(column_interface, Qt::AlignLeft);
        item->setTextAlignment(column_state, Qt::AlignCenter);
        for (int i=column_rx_frames; i<column_count; i++)
        {
            item->setTextAlignment(i, Qt::AlignRight);
        }

        ui->treeWidget->addTopLevelItem(item);
    }
    _stopTimer->stop();
    update();
    if (!_timer->isActive())
    {
        _timer->start(100);
    }
}

void CanStatusWindow::endMeasurement()
{
    update();
    _stopTimer->start(300);
}

void CanStatusWindow::clearStatistics()
{
    // Reset statistics in all active interfaces
    for (auto ifid : backend().getInterfaceList())
    {
        BusInterface *intf = backend().getInterfaceById(ifid);
        if (intf)
        {
            intf->resetStatistics();
            intf->updateStatistics();
        }
    }
    update();
}

void CanStatusWindow::update()
{
    for (QTreeWidgetItemIterator it(ui->treeWidget); *it; ++it)
    {
        QTreeWidgetItem *item = *it;
        BusInterface *intf = static_cast<BusInterface *>(item->data(0, Qt::UserRole).value<void *>());
        if (intf)
        {
            intf->updateStatistics();
            item->setText(column_state, intf->getStateText());
            item->setText(column_rx_frames, QString().number(intf->getNumRxFrames()));
            item->setText(column_rx_errors, QString().number(intf->getNumRxErrors()));
            item->setText(column_rx_overrun, QString().number(intf->getNumRxOverruns()));
            item->setText(column_tx_frames, QString().number(intf->getNumTxFrames()));
            item->setText(column_tx_errors, QString().number(intf->getNumTxErrors()));
            item->setText(column_tx_dropped, QString().number(intf->getNumTxDropped()));

            // Calculate Bus Load (%)
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            uint64_t currentBits = intf->getNumBits();
             if (_lastStats.contains(intf)) {
                 InterfaceStats &ls = _lastStats[intf];
                 qint64 dt = now - ls.lastTime;
                 if (dt >= 500) { // Update load every ~500ms for stability
                     uint64_t dbits = currentBits - ls.lastBits;
                     // value(key, default) would evaluate getBitrate() (a netlink query) on every tick
                     const auto effective = _effectiveBitrates.constFind(intf);
                     unsigned bitrate = (effective != _effectiveBitrates.constEnd()) ? effective.value() : intf->getBitrate();
                     if (bitrate > 0) {
                         double load = static_cast<double>(dbits) * 1000.0 / static_cast<double>(bitrate) / static_cast<double>(dt) * 100.0;
                         if (load > 100.0) load = 100.0;
                         item->setText(column_bus_load, QString("%1%").arg(load, 0, 'f', 1));
                     } else {
                         item->setText(column_bus_load, "---");
                     }
                     ls.lastBits = currentBits;
                     ls.lastTime = now;
                 }
             } else {
                 _lastStats[intf] = { currentBits, now };
                 item->setText(column_bus_load, "0.0%");
             }
             item->setText(column_bits, QString::number(currentBits));
        }
    }
}

Backend &CanStatusWindow::backend()
{
    return _backend;
}

QSize CanStatusWindow::sizeHint() const
{
    return QSize(1200, 600);
}
