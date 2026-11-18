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

#include <QDialog>
#include <QStringList>

#include "driver/CanDriver.h"

class Backend;
class BusInterface;
class QPushButton;
class QTextBrowser;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

/// Overview of every registered driver and the interfaces it currently
/// reports: bus type, open/closed, bus state, version, capabilities, supported
/// bitrates and the measurement network an interface is assigned to.
///
/// Items only store the BusInterfaceId and look the interface up on every
/// refresh, so a rescan that deletes interfaces never leaves a dangling pointer.
class HardwareDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HardwareDialog(Backend &backend, QWidget *parent = nullptr);

private:
    enum Column
    {
        column_device,
        column_bus,
        column_status,
        column_bus_state,
        column_version,
        column_capabilities,
        column_network,
        column_count
    };

    Backend &_backend;
    QTreeWidget *_tree;
    QTextBrowser *_details;
    QPushButton *_btnRescan;
    QTimer *_timer;

    BusInterfaceId _detailsId = InvalidBusInterfaceId;
    QStringList _detailsBitrates; ///< Cached per selection: may query the device

    void rebuild();
    void refreshStatus();
    void refreshDetails();
    void onSelectionChanged();
    void rescan();
    void copyReport();
    void updateRescanEnabled();

    [[nodiscard]] BusInterface *selectedInterface() const;
    [[nodiscard]] QString networkNameOf(BusInterfaceId id);

    [[nodiscard]] static QString busTypeText(const BusInterface &intf);
    [[nodiscard]] static QStringList capabilityNames(uint32_t caps);
};
