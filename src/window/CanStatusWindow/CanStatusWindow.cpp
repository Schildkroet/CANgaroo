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

#include <QDateTime>
#include <QHeaderView>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>

#include "core/Backend.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"
#include "driver/CanDriver.h"

namespace
{
constexpr int column_name  = 0;
constexpr int column_value = 1;

QString statLabel(CanStatusWindow::Stat stat)
{
    switch (stat)
    {
        case CanStatusWindow::Stat::RxFrames:  return QObject::tr("Rx Frames");
        case CanStatusWindow::Stat::RxErrors:  return QObject::tr("Rx Errors");
        case CanStatusWindow::Stat::RxOverrun: return QObject::tr("Rx Overrun");
        case CanStatusWindow::Stat::TxFrames:  return QObject::tr("Tx Frames");
        case CanStatusWindow::Stat::TxErrors:  return QObject::tr("Tx Errors");
        case CanStatusWindow::Stat::TxDropped: return QObject::tr("Tx Dropped");
        case CanStatusWindow::Stat::BusLoad:   return QObject::tr("Load (%)");
        case CanStatusWindow::Stat::Bits:      return QObject::tr("Bits");
        default:                               return {};
    }
}
}

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
        // Keep the last values on screen, but stop touching the interfaces.
        _rows.clear();
        _timer->stop();
    });

    ui->setupUi(this);
    ui->treeWidget->setColumnCount(2);
    ui->treeWidget->setHeaderLabels({tr("Name"), tr("Value")});
    ui->treeWidget->header()->setSectionResizeMode(column_name, QHeaderView::ResizeToContents);
    ui->treeWidget->header()->setStretchLastSection(true);
    ui->treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(ui->treeWidget, &QWidget::customContextMenuRequested, this, &CanStatusWindow::showContextMenu);
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
    ui->treeWidget->setHeaderLabels({tr("Name"), tr("Value")});

    for (const InterfaceRow &row : std::as_const(_rows))
    {
        for (size_t i = 0; i < row.stats.size(); ++i)
        {
            row.stats[i]->setText(column_name, statLabel(static_cast<Stat>(i)));
        }
    }
}

void CanStatusWindow::beginMeasurement()
{
    ui->treeWidget->clear();
    _rows.clear();
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

    QMap<CanDriver *, QTreeWidgetItem *> driverNodes;

    for (auto ifid : backend().getInterfaceList()) {
        BusInterface *intf = backend().getInterfaceById(ifid);
        if (!intf) continue;

        CanDriver *driver = intf->getDriver();
        QTreeWidgetItem *&driverNode = driverNodes[driver];
        if (!driverNode)
        {
            driverNode = new QTreeWidgetItem(ui->treeWidget);
            driverNode->setText(column_name, driver ? driver->getName() : tr("Unknown"));
            QFont f = driverNode->font(column_name);
            f.setBold(true);
            driverNode->setFont(column_name, f);
        }

        InterfaceRow row{intf, new QTreeWidgetItem(driverNode), {}};
        row.node->setText(column_name, intf->getName());
        for (size_t i = 0; i < row.stats.size(); ++i)
        {
            row.stats[i] = new QTreeWidgetItem(row.node);
            row.stats[i]->setText(column_name, statLabel(static_cast<Stat>(i)));
        }
        _rows.append(row);
    }

    ui->treeWidget->expandAll();
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

void CanStatusWindow::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = ui->treeWidget->itemAt(pos);
    if (!item)
    {
        return;
    }

    // Stat rows belong to their interface node.
    BusInterface *intf = nullptr;
    for (const InterfaceRow &row : std::as_const(_rows))
    {
        if (row.node == item || row.node == item->parent())
        {
            intf = row.intf;
            break;
        }
    }
    if (!intf)
    {
        return;
    }

    QMenu menu(this);
    QAction *recover = menu.addAction(tr("Recover from bus off"));
    recover->setEnabled((intf->getCapabilities() & BusInterface::capability_bus_off_recovery)
                        && intf->getState() == BusInterface::state_bus_off);
    if (menu.exec(ui->treeWidget->viewport()->mapToGlobal(pos)) == recover)
    {
        intf->recoverFromBusOff();
    }
}

void CanStatusWindow::update()
{
    auto setStat = [](const InterfaceRow &row, Stat stat, const QString &text)
    {
        row.stats[static_cast<size_t>(stat)]->setText(column_value, text);
    };

    for (const InterfaceRow &row : std::as_const(_rows))
    {
        BusInterface *intf = row.intf;
        intf->updateStatistics();
        row.node->setText(column_value, intf->getStateText());
        setStat(row, Stat::RxFrames,  QString::number(intf->getNumRxFrames()));
        setStat(row, Stat::RxErrors,  QString::number(intf->getNumRxErrors()));
        setStat(row, Stat::RxOverrun, QString::number(intf->getNumRxOverruns()));
        setStat(row, Stat::TxFrames,  QString::number(intf->getNumTxFrames()));
        setStat(row, Stat::TxErrors,  QString::number(intf->getNumTxErrors()));
        setStat(row, Stat::TxDropped, QString::number(intf->getNumTxDropped()));

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
                    setStat(row, Stat::BusLoad, QString("%1%").arg(load, 0, 'f', 1));
                } else {
                    setStat(row, Stat::BusLoad, "---");
                }
                ls.lastBits = currentBits;
                ls.lastTime = now;
            }
        } else {
            _lastStats[intf] = { currentBits, now };
            setStat(row, Stat::BusLoad, "0.0%");
        }
        setStat(row, Stat::Bits, QString::number(currentBits));
    }
}

Backend &CanStatusWindow::backend()
{
    return _backend;
}

QSize CanStatusWindow::sizeHint() const
{
    return QSize(400, 600);
}
