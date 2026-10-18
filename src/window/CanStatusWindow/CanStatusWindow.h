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

#pragma once

#include "core/ConfigurableWidget.h"
#include "driver/BusInterface.h"
#include <QEvent>
#include <QMap>
#include <QPoint>

#include <array>

namespace Ui {
class CanStatusWindow;
}

class Backend;
class QTimer;
class QTreeWidgetItem;

class CanStatusWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    /// Statistic rows shown below every interface node.
    enum class Stat {
        RxFrames, RxErrors, RxOverrun,
        TxFrames, TxErrors, TxDropped,
        BusLoad, Bits,
        Count
    };

public:
    explicit CanStatusWindow(QWidget *parent, Backend &backend);
    ~CanStatusWindow();

protected:
    void retranslateUi() override;

private slots:
    void beginMeasurement();
    void endMeasurement();
    void update();
    void clearStatistics();
    void showContextMenu(const QPoint &pos);

private:
    Ui::CanStatusWindow *ui;
    Backend &_backend;

    Backend &backend();
    QTimer *_timer;
    QTimer *_stopTimer;

    struct InterfaceStats {
        uint64_t lastBits;
        qint64 lastTime;
    };
    struct InterfaceRow {
        BusInterface *intf;
        QTreeWidgetItem *node;  ///< interface node; column 1 shows the bus state
        std::array<QTreeWidgetItem *, static_cast<size_t>(Stat::Count)> stats;
    };
    QList<InterfaceRow> _rows;

    QMap<void*, InterfaceStats> _lastStats;
    QMap<BusInterface*, unsigned> _effectiveBitrates;

    virtual QSize sizeHint() const override;
};
