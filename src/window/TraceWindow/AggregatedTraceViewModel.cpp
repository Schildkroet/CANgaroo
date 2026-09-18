/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "AggregatedTraceViewModel.h"
#include <QColor>
#include <QDateTime>
#include <QSet>
#include "core/ThemeManager.h"

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/LinFrame.h"

AggregatedTraceViewModel::AggregatedTraceViewModel(Backend &backend)
    : BaseTraceViewModel(backend)
{
    _rootItem = new AggregatedTraceViewItem(0);
    connect(backend.getTrace(), &BusTrace::afterAppend, this, &AggregatedTraceViewModel::onAfterAppend);
    connect(backend.getTrace(), &BusTrace::beforeRemove, this, &AggregatedTraceViewModel::onBeforeRemove);
    connect(backend.getTrace(), &BusTrace::beforeClear, this, &AggregatedTraceViewModel::beforeClear);
    connect(backend.getTrace(), &BusTrace::afterClear, this, &AggregatedTraceViewModel::afterClear);

    connect(&backend, &Backend::onSetupChanged, this, &AggregatedTraceViewModel::onSetupChanged);

    // Periodically repaint so stale-message fade updates without user interaction.
    // Rows older than the fade window are fully faded and never change color
    // again, so only contiguous spans of still-fading rows are repainted.
    connect(&_fadeTimer, &QTimer::timeout, this, [this]()
    {
        const int rows = _rootItem->childCount();
        if (rows <= 0 || !this->backend()->isMeasurementRunning())
        {
            return;
        }

        _fadeNowMs = QDateTime::currentMSecsSinceEpoch();

        // One extra tick of margin so rows get a final repaint at minimum alpha
        constexpr qint64 fade_window_ms = (255 - 80) * 1000 / 58 + 400;
        const int lastColumn = columnCount(QModelIndex()) - 1;

        int first = -1;
        for (int r = 0; r < rows; ++r)
        {
            const bool fading = (_fadeNowMs - _rootItem->child(r)->_lastmsg.getTimestamp_ms()) <= fade_window_ms;
            if (fading && first < 0)
            {
                first = r;
            }
            else if (!fading && first >= 0)
            {
                emit dataChanged(index(first, 0, QModelIndex()),
                                 index(r - 1, lastColumn, QModelIndex()),
                                 {Qt::ForegroundRole});
                first = -1;
            }
        }
        if (first >= 0)
        {
            emit dataChanged(index(first, 0, QModelIndex()),
                             index(rows - 1, lastColumn, QModelIndex()),
                             {Qt::ForegroundRole});
        }
    });
    _fadeTimer.start(200);
}

void AggregatedTraceViewModel::createItem(const BusMessage &msg)
{
    AggregatedTraceViewItem *item = new AggregatedTraceViewItem(_rootItem);
    item->_lastmsg = msg;

    if (msg.busType() == BusType::LIN) {
        LinFrame *linFrame = backend()->findLinFrame(msg);
        if (linFrame) {
            for (int i = 0; i < linFrame->signalList().size(); ++i) {
                item->appendChild(new AggregatedTraceViewItem(item));
            }
        }
    } else {
        CanDbMessage *dbmsg = backend()->findDbMessage(msg);
        if (dbmsg) {
            for (int i = 0; i < dbmsg->getSignals().length(); i++) {
                item->appendChild(new AggregatedTraceViewItem(item));
            }
        }
    }

    _rootItem->appendChild(item);
    _map[makeUniqueKey(msg)] = item;
}

void AggregatedTraceViewModel::updateItem(const BusMessage &msg)
{
    AggregatedTraceViewItem *item = _map.value(makeUniqueKey(msg));
    if (item) {
        item->_prevmsg = item->_lastmsg;
        item->_lastmsg = msg;
    }
}

void AggregatedTraceViewModel::onUpdateModel()
{

    if (!_pendingMessageInserts.isEmpty()) {
        beginInsertRows(QModelIndex(), _rootItem->childCount(), _rootItem->childCount()+_pendingMessageInserts.size()-1);
        for (const auto &msg : _pendingMessageInserts) {
            createItem(msg);
        }
        endInsertRows();
        _pendingMessageInserts.clear();
    }

    if (!_pendingMessageUpdates.isEmpty()) {
        QSet<int> updatedRows;
        for (const auto &msg : _pendingMessageUpdates) {
            AggregatedTraceViewItem *item = _map.value(makeUniqueKey(msg));
            if (item) {
                updateItem(msg);
                updatedRows.insert(item->row());
            }
        }

        for (auto r : updatedRows) {
            AggregatedTraceViewItem *item = _rootItem->child(r);
            if (item) {
                QModelIndex msgIdx = createIndex(r, 0, item);
                emit dataChanged(msgIdx, msgIdx.sibling(r, column_count - 1));

                if (item->childCount() > 0) {
                    QModelIndex firstChild = index(0, 0, msgIdx);
                    QModelIndex lastChild = index(item->childCount() - 1, column_count - 1, msgIdx);
                    emit dataChanged(firstChild, lastChild);
                }
            }
        }
        _pendingMessageUpdates.clear();
    }
}

void AggregatedTraceViewModel::onSetupChanged()
{
    beginResetModel();
    for (AggregatedTraceViewItem *item : _map.values()) {
        item->removeChildren();
        const BusMessage &lastMsg = item->_lastmsg;
        if (lastMsg.busType() == BusType::LIN) {
            LinFrame *linFrame = backend()->findLinFrame(lastMsg);
            if (linFrame) {
                for (int i = 0; i < linFrame->signalList().size(); ++i) {
                    item->appendChild(new AggregatedTraceViewItem(item));
                }
            }
        } else {
            CanDbMessage *dbmsg = backend()->findDbMessage(lastMsg);
            if (dbmsg) {
                for (int i = 0; i < dbmsg->getSignals().length(); i++) {
                    item->appendChild(new AggregatedTraceViewItem(item));
                }
            }
        }
    }
    endResetModel();
}

void AggregatedTraceViewModel::onAfterAppend()
{
    BusTrace *trace = backend()->getTrace();
    int size = trace->size();
    if (_lastProcessedIndex >= size) {
        _lastProcessedIndex = size - 1;
    }

    for (int i=_lastProcessedIndex + 1; i<size; i++) {
        BusMessage msg = trace->getMessage(i);
        unique_key_t key = makeUniqueKey(msg);
        if (_map.contains(key) || _pendingMessageInserts.contains(key)) {
            _pendingMessageUpdates.append(msg);
        } else {
            _pendingMessageInserts[key] = msg;
        }
        _lastProcessedIndex = i;
    }

    onUpdateModel();
}

void AggregatedTraceViewModel::onBeforeRemove(int count)
{
    // BusTrace prunes from the front, so already-processed indices shift down
    _lastProcessedIndex -= count;
    if (_lastProcessedIndex < -1) {
        _lastProcessedIndex = -1;
    }
}

void AggregatedTraceViewModel::beforeClear()
{
    beginResetModel();
    delete _rootItem;
    _map.clear();
    _pendingMessageInserts.clear();
    _pendingMessageUpdates.clear();
    _lastProcessedIndex = -1;
    _rootItem = new AggregatedTraceViewItem(0);
}

void AggregatedTraceViewModel::afterClear()
{
    endResetModel();
}

AggregatedTraceViewModel::unique_key_t AggregatedTraceViewModel::makeUniqueKey(const BusMessage &msg) const
{
    // Bit 63: RX flag; bit 62: error frame; bits 32-47: interface ID; bit 30: bus type (1=LIN);
    // bits 0-29: frame ID. Error frames have ID 0, the error bit keeps them apart from real 0x000.
    return  static_cast<uint64_t>(msg.isRX()) << 63
          | static_cast<uint64_t>(msg.isErrorFrame()) << 62
          | static_cast<uint64_t>(msg.getInterfaceId()) << 32
          | static_cast<uint64_t>(static_cast<uint8_t>(msg.busType())) << 30
          | (static_cast<uint64_t>(msg.getRawId()) & 0x3FFFFFFFull);
}

QModelIndex AggregatedTraceViewModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    const AggregatedTraceViewItem *parentItem = parent.isValid() ? static_cast<AggregatedTraceViewItem*>(parent.internalPointer()) : _rootItem;
    const AggregatedTraceViewItem *childItem = parentItem->child(row);

    if (childItem) {
        return createIndex(row, column, const_cast<AggregatedTraceViewItem*>(childItem));
    } else {
        return QModelIndex();
    }
}

QModelIndex AggregatedTraceViewModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    AggregatedTraceViewItem *childItem = static_cast<AggregatedTraceViewItem*>(index.internalPointer());
    AggregatedTraceViewItem *parentItem = childItem->parent();

    if (parentItem == _rootItem) {
        return QModelIndex();
    }

    return createIndex(parentItem->row(), 0, parentItem);
}

int AggregatedTraceViewModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) {
        return 0;
    }

    AggregatedTraceViewItem *parentItem;
    if (parent.isValid()) {
        parentItem = static_cast<AggregatedTraceViewItem*>(parent.internalPointer());
    } else {
        parentItem = _rootItem;
    }
    return parentItem->childCount();
}

BusMessage AggregatedTraceViewModel::getMessage(const QModelIndex &index) const
{
    if (!index.isValid()) return BusMessage();
    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem*>(index.internalPointer());
    if (item == _rootItem) return BusMessage();
    return (item->parent() == _rootItem) ? item->_lastmsg : item->parent()->_lastmsg;
}

QVariant AggregatedTraceViewModel::data_DisplayRole(const QModelIndex &index, int role) const
{
    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem *>(index.internalPointer());
    if (!item) { return QVariant(); }

    if (index.column() == column_index) {
        return (item->parent() == _rootItem) ? QVariant(static_cast<uint32_t>(index.row() + 1)) : QVariant();
    }

    if (item->parent() == _rootItem) { // BusMessage row
        return data_DisplayRole_Message(index, role, item->_lastmsg, item->_prevmsg);
    } else { // CanSignal Row
        return data_DisplayRole_Signal(index, role, item->parent()->_lastmsg);
    }
}

QVariant AggregatedTraceViewModel::data_ChangedBytesRole(const QModelIndex &index) const
{
    if (index.column() != column_data) { return QVariant(); }

    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem *>(index.internalPointer());
    if (!item || item->parent() != _rootItem) { return QVariant(); }

    const BusMessage &cur  = item->_lastmsg;
    const BusMessage &prev = item->_prevmsg;
    if (prev.getLength() == 0) { return QVariant(); }
    if (cur.isErrorFrame() || prev.isErrorFrame()) { return QVariant(); } // text is error flags, not bytes

    uint64_t mask = 0;
    const int len = qMin(cur.getLength(), prev.getLength());
    for (int i = 0; i < len; ++i) {
        if (cur.getData()[i] != prev.getData()[i])
            mask |= (1ULL << i);
    }
    for (int i = len; i < cur.getLength(); ++i)
        mask |= (1ULL << i);

    return QVariant::fromValue(mask);
}

QVariant AggregatedTraceViewModel::data_TextColorRole(const QModelIndex &index, int role) const
{
    (void) role;
    bool isDark = ThemeManager::instance().isDarkMode();

    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem *>(index.internalPointer());
    if (!item) { return QVariant(); }

    const BusMessage &msg = (item->parent() == _rootItem)
        ? item->_lastmsg
        : item->parent()->_lastmsg;

    // Fade stale messages via alpha based on time since last reception, but
    // only while the measurement is running - once it's stopped no more
    // messages will arrive to refresh timestamps, so every row would just
    // decay to minimum alpha over a few seconds and stay unreadable.
    int alpha = 255;
    if (backend()->isMeasurementRunning())
    {
        // Precondition: message timestamps must be Unix-epoch microseconds so
        // that getTimestamp_ms() is directly comparable to currentMSecsSinceEpoch().
        qint64 now_ms = _fadeNowMs > 0 ? _fadeNowMs : QDateTime::currentMSecsSinceEpoch();
        double diff_sec = (now_ms - msg.getTimestamp_ms()) / 1000.0;

        alpha = 255 - static_cast<int>(diff_sec * 58);
        alpha = qBound(80, alpha, 255);
    }

    QColor color = msg.isErrorFrame()
        ? (isDark ? QColor(255, 100, 100) : QColor(Qt::red))
        : ThemeManager::instance().colors().text;

    color.setAlpha(alpha);
    return color;
}


