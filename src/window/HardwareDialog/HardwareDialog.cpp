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

#include "HardwareDialog.h"

#include <array>
#include <chrono>
#include <utility>

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/Backend.h"
#include "core/MeasurementInterface.h"
#include "core/MeasurementNetwork.h"
#include "driver/BusInterface.h"

using namespace std::chrono_literals;

namespace
{

constexpr int RoleInterfaceId = Qt::UserRole + 1;

struct CapabilityName
{
    uint32_t flag;
    const char *name;
};

// Order: CAN first, then LIN, as in BusInterface's capability enum.
constexpr std::array capabilityTable{
    CapabilityName{BusInterface::capability_canfd,                QT_TRANSLATE_NOOP("HardwareDialog", "CAN FD")},
    CapabilityName{BusInterface::capability_listen_only,          QT_TRANSLATE_NOOP("HardwareDialog", "Listen only")},
    CapabilityName{BusInterface::capability_triple_sampling,      QT_TRANSLATE_NOOP("HardwareDialog", "Triple sampling")},
    CapabilityName{BusInterface::capability_one_shot,             QT_TRANSLATE_NOOP("HardwareDialog", "One shot")},
    CapabilityName{BusInterface::capability_auto_restart,         QT_TRANSLATE_NOOP("HardwareDialog", "Auto restart")},
    CapabilityName{BusInterface::capability_config_os,            QT_TRANSLATE_NOOP("HardwareDialog", "Configurable timestamps")},
    CapabilityName{BusInterface::capability_custom_bitrate,       QT_TRANSLATE_NOOP("HardwareDialog", "Custom bitrate")},
    CapabilityName{BusInterface::capability_custom_canfd_bitrate, QT_TRANSLATE_NOOP("HardwareDialog", "Custom CAN FD bitrate")},
    CapabilityName{BusInterface::capability_bus_off_recovery,     QT_TRANSLATE_NOOP("HardwareDialog", "Bus-off recovery")},
    CapabilityName{BusInterface::capability_tdc,                  QT_TRANSLATE_NOOP("HardwareDialog", "TDC")},
    CapabilityName{BusInterface::capability_lin_master,           QT_TRANSLATE_NOOP("HardwareDialog", "LIN master")},
    CapabilityName{BusInterface::capability_lin_slave,            QT_TRANSLATE_NOOP("HardwareDialog", "LIN slave")},
    CapabilityName{BusInterface::capability_lin_monitor,          QT_TRANSLATE_NOOP("HardwareDialog", "LIN monitor")},
};

void setTextIfChanged(QTreeWidgetItem *item, int column, const QString &text)
{
    if (item->text(column) != text)
        item->setText(column, text);
}

} // namespace

HardwareDialog::HardwareDialog(Backend &backend, QWidget *parent)
    : QDialog(parent),
      _backend(backend),
      _tree(new QTreeWidget(this)),
      _details(new QTextBrowser(this)),
      _btnRescan(new QPushButton(tr("&Rescan"), this)),
      _timer(new QTimer(this))
{
    setWindowTitle(tr("Hardware"));
    resize(1000, 600);

    _tree->setColumnCount(column_count);
    _tree->setHeaderLabels({tr("Device"), tr("Bus"), tr("Status"), tr("Bus State"),
                            tr("Version"), tr("Capabilities"), tr("Network")});
    _tree->setRootIsDecorated(true);
    _tree->setUniformRowHeights(true);
    _tree->setAlternatingRowColors(true);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    _tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    _tree->header()->setStretchLastSection(true);

    _details->setOpenLinks(false);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(_tree);
    splitter->addWidget(_details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    _btnRescan->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    _btnRescan->setToolTip(tr("Ask every driver to look for new or removed devices"));
    buttons->addButton(_btnRescan, QDialogButtonBox::ActionRole);
    auto *btnCopy = buttons->addButton(tr("&Copy Report"), QDialogButtonBox::ActionRole);
    btnCopy->setToolTip(tr("Copy the device list as plain text, e.g. for a bug report"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_btnRescan, &QPushButton::clicked, this, &HardwareDialog::rescan);
    connect(btnCopy, &QPushButton::clicked, this, &HardwareDialog::copyReport);
    connect(_tree, &QTreeWidget::itemSelectionChanged, this, &HardwareDialog::onSelectionChanged);

    // A rescan or a new setup can delete interfaces or move them between networks.
    connect(&_backend, &Backend::onSetupChanged, this, &HardwareDialog::rebuild);
    connect(&_backend, &Backend::beginMeasurement, this, &HardwareDialog::updateRescanEnabled);
    connect(&_backend, &Backend::endMeasurement, this, &HardwareDialog::updateRescanEnabled);

    // Open/closed and bus state change with the measurement; poll them.
    _timer->setInterval(1s);
    connect(_timer, &QTimer::timeout, this, [this]()
    {
        refreshStatus();
        refreshDetails();
    });
    _timer->start();

    updateRescanEnabled();
    rebuild();
}

void HardwareDialog::rebuild()
{
    const BusInterfaceId selectedId = _detailsId;

    _tree->clear();
    QTreeWidgetItem *toSelect = nullptr;

    for (CanDriver *driver : _backend.getDrivers())
    {
        auto *driverItem = new QTreeWidgetItem(_tree);
        driverItem->setText(column_device, driver->getName());
        QFont bold = driverItem->font(column_device);
        bold.setBold(true);
        driverItem->setFont(column_device, bold);

        const auto interfaces = driver->getInterfaces();
        if (interfaces.isEmpty())
        {
            driverItem->setText(column_status, tr("no devices found"));
            driverItem->setForeground(column_status, palette().brush(QPalette::Disabled, QPalette::Text));
        }

        for (BusInterface *intf : interfaces)
        {
            auto *item = new QTreeWidgetItem(driverItem);
            item->setData(column_device, RoleInterfaceId, intf->getId());
            item->setText(column_device, intf->getName());
            item->setToolTip(column_device, intf->getDetailsStr());
            item->setText(column_bus, busTypeText(*intf));

            const QStringList caps = capabilityNames(intf->getCapabilities());
            item->setText(column_capabilities, caps.join(QStringLiteral(", ")));
            item->setToolTip(column_capabilities, caps.join(QLatin1Char('\n')));

            if (intf->getId() == selectedId)
                toSelect = item;
        }
        driverItem->setExpanded(true);
    }

    refreshStatus();

    for (int col = 0; col < column_count - 1; ++col)
        _tree->resizeColumnToContents(col);

    if (toSelect)
    {
        _tree->setCurrentItem(toSelect);
    }
    else
    {
        _detailsId = InvalidBusInterfaceId;
        onSelectionChanged();
    }
}

void HardwareDialog::refreshStatus()
{
    for (int d = 0; d < _tree->topLevelItemCount(); ++d)
    {
        QTreeWidgetItem *driverItem = _tree->topLevelItem(d);
        for (int i = 0; i < driverItem->childCount(); ++i)
        {
            QTreeWidgetItem *item = driverItem->child(i);
            const auto id = static_cast<BusInterfaceId>(item->data(column_device, RoleInterfaceId).toUInt());
            BusInterface *intf = _backend.getInterfaceById(id);
            if (!intf)
                continue; // removed; onSetupChanged() rebuilds the tree

            const bool open = intf->isOpen();
            setTextIfChanged(item, column_status, open ? tr("Open") : tr("Available"));
            setTextIfChanged(item, column_bus_state, open ? intf->getStateText() : QString());
            setTextIfChanged(item, column_version, intf->getVersion());
            setTextIfChanged(item, column_network, networkNameOf(id));

            const uint32_t state = open ? intf->getState() : static_cast<uint32_t>(BusInterface::state_unknown);
            QBrush brush = palette().brush(QPalette::Text);
            if (state == BusInterface::state_bus_off || state == BusInterface::state_passive)
                brush = QBrush(Qt::red);
            else if (state == BusInterface::state_warning)
                brush = QBrush(QColor(0xE0, 0x90, 0x00));
            item->setForeground(column_bus_state, brush);
        }
    }
}

void HardwareDialog::onSelectionChanged()
{
    BusInterface *intf = selectedInterface();
    const BusInterfaceId id = intf ? intf->getId() : InvalidBusInterfaceId;

    // The bitrate list may come from the device; fetch it once per selection.
    if (id != _detailsId)
    {
        _detailsId = id;
        _detailsBitrates.clear();
        if (intf && intf->busType() == BusType::CAN)
        {
            for (const CanTiming &timing : intf->getAvailableBitrates())
                _detailsBitrates.append(timing.getTitle());
            _detailsBitrates.removeDuplicates();
        }
        _details->clear();
        _details->setProperty("lastHtml", QString());
    }
    refreshDetails();
}

void HardwareDialog::refreshDetails()
{
    BusInterface *intf = _backend.getInterfaceById(_detailsId);
    if (_detailsId == InvalidBusInterfaceId || !intf)
    {
        const QString hint = tr("Select an interface to see its details.");
        if (_details->toPlainText() != hint)
            _details->setPlainText(hint);
        _details->setProperty("lastHtml", QString());
        return;
    }

    auto row = [](const QString &key, const QString &value)
    {
        return QStringLiteral("<tr><td style='padding-right:16px'><b>%1</b></td><td>%2</td></tr>")
            .arg(key.toHtmlEscaped(), value.isEmpty() ? QStringLiteral("&ndash;") : value.toHtmlEscaped());
    };

    const bool open = intf->isOpen();
    QString html = QStringLiteral("<h3>%1</h3><table>").arg(intf->getName().toHtmlEscaped());
    html += row(tr("Driver"), intf->getDriver() ? intf->getDriver()->getName() : QString());
    html += row(tr("Description"), intf->getDetailsStr());
    html += row(tr("Bus"), busTypeText(*intf));
    html += row(tr("Version"), intf->getVersion());
    html += row(tr("Status"), open ? tr("Open") : tr("Available"));
    if (open)
        html += row(tr("Bus state"), intf->getStateText());
    html += row(tr("Interface ID"), QString::number(intf->getId()));

    // How the current setup uses this interface.
    for (MeasurementNetwork *network : _backend.getSetup().getNetworks())
    {
        for (MeasurementInterface *mi : network->interfaces())
        {
            if (mi->busInterface() != intf->getId())
                continue;
            QString usage = network->name();
            if (mi->busType() == BusType::LIN)
                usage += tr(", %1 Bd").arg(mi->linBaudRate());
            else if (mi->isCanFD())
                usage += tr(", %1 / %2 kBit/s (FD)").arg(mi->bitrate() / 1000).arg(mi->fdBitrate() / 1000);
            else
                usage += tr(", %1 kBit/s").arg(mi->bitrate() / 1000);
            if (mi->isListenOnlyMode())
                usage += tr(", listen only");
            if (!mi->isEnabled())
                usage += tr(" (disabled)");
            html += row(tr("Setup"), usage);
        }
    }
    html += QStringLiteral("</table>");

    const uint32_t caps = intf->getCapabilities();
    html += QStringLiteral("<h4>%1</h4><table>").arg(tr("Capabilities").toHtmlEscaped());
    for (const auto &[flag, name] : capabilityTable)
    {
        const bool has = (caps & flag) != 0;
        html += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>")
                    .arg(has ? QStringLiteral("&#10003;") : QStringLiteral("&ndash;"),
                         has ? tr(name).toHtmlEscaped()
                             : QStringLiteral("<span style='color:gray'>%1</span>").arg(tr(name).toHtmlEscaped()));
    }
    html += QStringLiteral("</table>");

    if (!_detailsBitrates.isEmpty())
    {
        html += QStringLiteral("<h4>%1</h4><ul>").arg(tr("Supported bitrates (%1)").arg(_detailsBitrates.size()).toHtmlEscaped());
        for (const QString &br : std::as_const(_detailsBitrates))
            html += QStringLiteral("<li>%1</li>").arg(br.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }

    // Rebuilding the document every tick would reset the scroll position.
    if (html == _details->property("lastHtml").toString())
        return;
    _details->setProperty("lastHtml", html);
    const int scroll = _details->verticalScrollBar()->value();
    _details->setHtml(html);
    _details->verticalScrollBar()->setValue(scroll);
}

void HardwareDialog::rescan()
{
    if (_backend.isMeasurementRunning())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    _backend.refreshInterfaces(); // emits onSetupChanged() -> rebuild()
    QApplication::restoreOverrideCursor();
}

void HardwareDialog::copyReport()
{
    QStringList lines;
    for (CanDriver *driver : _backend.getDrivers())
    {
        lines << driver->getName();
        const auto interfaces = driver->getInterfaces();
        if (interfaces.isEmpty())
            lines << QStringLiteral("  (no devices found)");
        for (BusInterface *intf : interfaces)
        {
            lines << QStringLiteral("  %1 [%2] %3, version %4%5")
                         .arg(intf->getName(), busTypeText(*intf),
                              intf->isOpen() ? QStringLiteral("open, ") + intf->getStateText() : QStringLiteral("closed"),
                              intf->getVersion(),
                              intf->getDetailsStr().isEmpty() ? QString() : QStringLiteral(" - ") + intf->getDetailsStr());
            const QStringList caps = capabilityNames(intf->getCapabilities());
            if (!caps.isEmpty())
                lines << QStringLiteral("    capabilities: ") + caps.join(QStringLiteral(", "));
        }
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
}

void HardwareDialog::updateRescanEnabled()
{
    const bool running = _backend.isMeasurementRunning();
    _btnRescan->setEnabled(!running);
    _btnRescan->setToolTip(running ? tr("Stop the measurement to rescan")
                                   : tr("Ask every driver to look for new or removed devices"));
}

BusInterface *HardwareDialog::selectedInterface() const
{
    const QTreeWidgetItem *item = _tree->currentItem();
    if (!item || !item->isSelected())
        return nullptr;
    const QVariant id = item->data(column_device, RoleInterfaceId);
    if (!id.isValid())
        return nullptr; // driver row
    return _backend.getInterfaceById(static_cast<BusInterfaceId>(id.toUInt()));
}

QString HardwareDialog::networkNameOf(BusInterfaceId id)
{
    QStringList names;
    for (MeasurementNetwork *network : _backend.getSetup().getNetworks())
    {
        for (MeasurementInterface *mi : network->interfaces())
        {
            if (mi->busInterface() == id)
                names << network->name();
        }
    }
    return names.join(QStringLiteral(", "));
}

QString HardwareDialog::busTypeText(const BusInterface &intf)
{
    return intf.busType() == BusType::LIN ? QStringLiteral("LIN") : QStringLiteral("CAN");
}

QStringList HardwareDialog::capabilityNames(uint32_t caps)
{
    QStringList names;
    for (const auto &[flag, name] : capabilityTable)
    {
        if (caps & flag)
            names << tr(name);
    }
    return names;
}
