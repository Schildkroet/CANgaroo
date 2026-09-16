#include "UnifiedTraceViewModel.h"
#include "core/BusTrace.h"
#include "core/Backend.h"
#include "core/DBC/LinFrame.h"
#include "core/DBC/CanDbMessage.h"
#include <QColor>
#include <QSet>
#include <QDateTime>
#include <QSettings>
#include "core/ThemeManager.h"

void UnifiedTraceViewModel::applyProtocolConfig()
{
    QSettings s;
    m_protocolManager.config().enableUds29Bit = s.value("decoder/uds29Bit", true).toBool();
}

UnifiedTraceViewModel::UnifiedTraceViewModel(Backend &backend, Category category)
    : BaseTraceViewModel(backend), m_category(category), m_aggregating(category == Cat_J1939)
{
    applyProtocolConfig();
    m_rootItem = std::make_shared<UnifiedTraceItem>(BusMessage()); // Dummy root
    m_firstTimestamp = 0;
    m_previousRowTimestamp = 0;
    m_globalIndexCounter = 1;

    m_processTimer.setInterval(150);
    m_processTimer.setSingleShot(true);
    connect(&m_processTimer, &QTimer::timeout, this, &UnifiedTraceViewModel::processNewMessages);

    connect(backend.getTrace(), &BusTrace::afterAppend, this, &UnifiedTraceViewModel::onTraceDirty);
    connect(backend.getTrace(), &BusTrace::beforeRemove, this, &UnifiedTraceViewModel::beforeRemove);
    connect(backend.getTrace(), &BusTrace::afterRemove, this, &UnifiedTraceViewModel::afterRemove);
    connect(backend.getTrace(), &BusTrace::beforeClear, this, &UnifiedTraceViewModel::beforeClear);
    connect(backend.getTrace(), &BusTrace::afterClear, this, &UnifiedTraceViewModel::afterClear);
    connect(&backend, &Backend::onSetupChanged, this, &UnifiedTraceViewModel::onSetupChanged);
}

UnifiedTraceViewModel::~UnifiedTraceViewModel()
{
}

QModelIndex UnifiedTraceViewModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();

    UnifiedTraceItem *parentItem;
    if (!parent.isValid())
        parentItem = m_rootItem.get();
    else
        parentItem = static_cast<UnifiedTraceItem*>(parent.internalPointer());

    std::shared_ptr<UnifiedTraceItem> childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem.get());
    else
        return QModelIndex();
}

QModelIndex UnifiedTraceViewModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return QModelIndex();

    UnifiedTraceItem *childItem = static_cast<UnifiedTraceItem*>(child.internalPointer());
    UnifiedTraceItem *parentItem = childItem->parentItem();

    if (parentItem == m_rootItem.get())
        return QModelIndex();

    return createIndex(parentItem->row(), 0, parentItem);
}

int UnifiedTraceViewModel::rowCount(const QModelIndex &parent) const
{
    UnifiedTraceItem *parentItem;
    if (parent.column() > 0)
        return 0;

    if (!parent.isValid())
        parentItem = m_rootItem.get();
    else
        parentItem = static_cast<UnifiedTraceItem*>(parent.internalPointer());

    return parentItem->childCount();
}

int UnifiedTraceViewModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return BaseTraceViewModel::column_count;
}

bool UnifiedTraceViewModel::hasChildren(const QModelIndex &parent) const
{
    return rowCount(parent) > 0;
}

BusMessage UnifiedTraceViewModel::getMessage(const QModelIndex &index) const
{
    if (!index.isValid()) return BusMessage();
    UnifiedTraceItem *item = static_cast<UnifiedTraceItem*>(index.internalPointer());
    if (item->isProtocol()) {
        const ProtocolMessage& pmsg = item->protocolMessage();
        return pmsg.rawFrames.isEmpty() ? BusMessage() : pmsg.rawFrames.first();
    } else if (item->isSignal()) {
        UnifiedTraceItem *parent = item->parentItem();
        return parent ? parent->rawFrame() : BusMessage();
    } else if (item->isMetadata()) {
        UnifiedTraceItem *parent = item->parentItem();
        if (parent && parent->isProtocol()) {
            const ProtocolMessage& pmsg = parent->protocolMessage();
            return pmsg.rawFrames.isEmpty() ? BusMessage() : pmsg.rawFrames.first();
        }
        return BusMessage();
    } else {
        return item->rawFrame();
    }
}

QVariant UnifiedTraceViewModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    switch (role) {
        case Qt::DisplayRole:
            return data_DisplayRole(index, role);
        case Qt::ForegroundRole:
            return data_TextColorRole(index, role);
        case Qt::TextAlignmentRole:
            return BaseTraceViewModel::data_TextAlignmentRole(index, role);
        case ChangedBytesRole:
        {
            if (index.column() != column_data) { return QVariant(); }
            UnifiedTraceItem *item = static_cast<UnifiedTraceItem*>(index.internalPointer());
            if (!item || item->isProtocol() || item->isMetadata() || item->isSignal()) { return QVariant(); }
            const BusMessage &cur  = item->rawFrame();
            const BusMessage &prev = item->prevSameIdFrame();
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
        default:
            return BaseTraceViewModel::data(index, role);
    }
}

void UnifiedTraceViewModel::onTraceDirty()
{
    if (!m_processTimer.isActive()) {
        m_processTimer.start();
    }
}

void UnifiedTraceViewModel::processNewMessages()
{
    BusTrace *trace = backend()->getTrace();
    int size = trace->size();
    QSet<int> updatedRows;
    if (m_lastProcessedIndex >= size) {
        m_lastProcessedIndex = size - 1;
    }

    QList<std::shared_ptr<UnifiedTraceItem>> newItems;

    for (int i = m_lastProcessedIndex + 1; i < size; ++i) {
        BusMessage msg = trace->getMessage(i);

        // A loaded DBC message takes priority over the generic protocol decoders
        // in the combined "All" view, so e.g. a J1939-shaped frame covered by the
        // DBC is shown with its DBC signal names instead of the raw PGN decode.
        bool preferDbc = m_category == Cat_All && backend()->findDbMessage(msg) != nullptr;

        ProtocolMessage pmsg;
        DecodeStatus status = preferDbc ? DecodeStatus::Ignored : m_protocolManager.processFrame(msg, pmsg);

        bool shouldAppend = false;
        if (status == DecodeStatus::Completed) {
            if (pmsg.protocol.compare("uds", Qt::CaseInsensitive) == 0 && m_category == Cat_UDS) {
                if (m_aggregating) {
                    uint64_t key = getUdsKey(pmsg);
                    if (m_udsAggregatedMap.count(key)) {
                        auto &item = m_udsAggregatedMap[key];
                        item->updateProtocolMessage(pmsg);
                        updatedRows.insert(item->row());
                    } else {
                        shouldAppend = true;
                    }
                } else {
                    shouldAppend = true;
                }
            } else if (pmsg.protocol.compare("j1939", Qt::CaseInsensitive) == 0 && m_category == Cat_J1939) {
                if (m_aggregating) {
                    uint64_t key = getJ1939Key(pmsg);
                    if (m_j1939AggregatedMap.count(key)) {
                        auto &item = m_j1939AggregatedMap[key];
                        item->updateProtocolMessage(pmsg);
                        updatedRows.insert(item->row());
                    } else {
                        shouldAppend = true;
                    }
                } else {
                    shouldAppend = true;
                }
            }

            if (shouldAppend) {
                auto item = std::make_shared<UnifiedTraceItem>(pmsg, m_rootItem.get());
                if (m_firstTimestamp == 0) m_firstTimestamp = pmsg.timestamp;
                item->setTimestamp(pmsg.timestamp);
                item->setGlobalIndex(m_globalIndexCounter++);

                uint64_t dkey = makeDeltaKey(pmsg);
                item->setPrevSameIdTimestamp(m_prevTimestampByKey.value(dkey, 0));
                m_prevTimestampByKey[dkey] = pmsg.timestamp;

                newItems.append(item);

                if (m_category == Cat_J1939 && m_aggregating) {
                    m_j1939AggregatedMap[getJ1939Key(pmsg)] = item;
                } else if (m_category == Cat_UDS && m_aggregating) {
                    m_udsAggregatedMap[getUdsKey(pmsg)] = item;
                }
            }
        }

        // The Monitor ("All") tab is a raw trace view: every physical frame shows
        // up here regardless of whether a higher-level protocol decoder consumed
        // or completed it. Decoded protocol summaries belong only in the
        // dedicated UDS/J1939 tabs, handled above.
        if (m_category == Cat_All) {
            auto item = std::make_shared<UnifiedTraceItem>(msg, m_rootItem.get());
            uint64_t ts = static_cast<uint64_t>(msg.getFloatTimestamp() * 1000000.0);
            if (m_firstTimestamp == 0) m_firstTimestamp = ts;
            item->setTimestamp(ts);
            item->setGlobalIndex(m_globalIndexCounter++);

            uint64_t dkey = makeDeltaKey(msg);
            item->setPrevSameIdTimestamp(m_prevTimestampByKey.value(dkey, 0));
            m_prevTimestampByKey[dkey] = ts;

            item->setPrevSameIdFrame(m_prevMessageByKey.value(dkey));
            m_prevMessageByKey[dkey] = msg;

            if (msg.busType() == BusType::LIN) {
                LinFrame *linFrame = backend()->findLinFrame(msg);
                if (linFrame) {
                    for (int s = 0; s < linFrame->signalList().size(); ++s)
                        item->appendChild(std::make_shared<UnifiedTraceItem>(s, item.get()));
                }
            } else {
                CanDbMessage *dbmsg = backend()->findDbMessage(msg);
                if (dbmsg) {
                    for (int s = 0; s < dbmsg->getSignals().length(); ++s)
                        item->appendChild(std::make_shared<UnifiedTraceItem>(s, item.get()));
                }
            }

            newItems.append(item);
        }
        m_lastProcessedIndex = i;
    }

    if (!newItems.isEmpty()) {
        beginInsertRows(QModelIndex(), m_rootItem->childCount(), m_rootItem->childCount() + newItems.size() - 1);
        for (auto &item : newItems) {
            m_rootItem->appendChild(item);
        }
        endInsertRows();
    }

    // Emit batched dataChanged signals for aggregated rows
    for (int row : updatedRows) {
        auto item = m_rootItem->child(row);
        if (item) {
            QModelIndex idx = createIndex(row, 0, item.get());
            emit dataChanged(idx, idx.sibling(row, column_count - 1));

            // Also notify children updates if expanded
            if (item->childCount() > 0) {
                QModelIndex firstChild = index(0, 0, idx);
                QModelIndex lastChild = index(item->childCount() - 1, column_count - 1, idx);
                emit dataChanged(firstChild, lastChild);
            }
        }
    }

    // Hard row limit check for the UI model
    if (m_rootItem->childCount() > m_maxRows) {
        int toRemove = m_maxRows / 5; // Remove 20% to avoid frequent trimming
        if (toRemove > 0) {
            beginRemoveRows(QModelIndex(), 0, toRemove - 1);
            m_rootItem->removeChildren(0, toRemove);

            // Update row indices for remaining children
            for (int i = 0; i < m_rootItem->childCount(); ++i) {
                m_rootItem->child(i)->setRow(i);
            }
            endRemoveRows();

            if (m_category == Cat_J1939 && m_aggregating) {
                m_j1939AggregatedMap.clear();
                for (int i = 0; i < m_rootItem->childCount(); ++i) {
                    auto child = m_rootItem->child(i);
                    if (child && child->isProtocol() && child->protocolMessage().protocol.compare("j1939", Qt::CaseInsensitive) == 0) {
                        m_j1939AggregatedMap[getJ1939Key(child->protocolMessage())] = child;
                    }
                }
            } else if (m_category == Cat_UDS && m_aggregating) {
                m_udsAggregatedMap.clear();
                for (int i = 0; i < m_rootItem->childCount(); ++i) {
                    auto child = m_rootItem->child(i);
                    if (child && child->isProtocol() && child->protocolMessage().protocol.compare("uds", Qt::CaseInsensitive) == 0) {
                        m_udsAggregatedMap[getUdsKey(child->protocolMessage())] = child;
                    }
                }
            }
        }
    }
}

void UnifiedTraceViewModel::beforeRemove(int count)
{
    // Adjust lastProcessedIndex since BusTrace removed items from the front
    m_lastProcessedIndex -= count;
    if (m_lastProcessedIndex < -1) m_lastProcessedIndex = -1;
}

void UnifiedTraceViewModel::afterRemove(int count)
{
    // Nothing more needed here specifically for m_lastProcessedIndex
    Q_UNUSED(count);
}

void UnifiedTraceViewModel::beforeClear()
{
    beginResetModel();
}

void UnifiedTraceViewModel::afterClear()
{
    m_processTimer.stop();
    m_rootItem = std::make_shared<UnifiedTraceItem>(BusMessage());
    m_protocolManager.reset();
    m_lastProcessedIndex = -1;
    m_globalIndexCounter = 1;
    m_firstTimestamp = 0;
    m_previousRowTimestamp = 0;
    m_j1939AggregatedMap.clear();
    m_udsAggregatedMap.clear();
    m_prevTimestampByKey.clear();
    m_prevMessageByKey.clear();
    endResetModel();
}

void UnifiedTraceViewModel::onSetupChanged()
{
    m_processTimer.stop();
    beginResetModel();
    m_rootItem = std::make_shared<UnifiedTraceItem>(BusMessage());
    m_protocolManager.reset();
    applyProtocolConfig();
    m_lastProcessedIndex = -1;
    m_globalIndexCounter = 1;
    m_firstTimestamp = 0;
    m_previousRowTimestamp = 0;
    m_j1939AggregatedMap.clear();
    m_udsAggregatedMap.clear();
    m_prevTimestampByKey.clear();
    m_prevMessageByKey.clear();

    // Re-process all frames from trace buffer
    // We bypass beginInsertRows by being inside beginResetModel
    processNewMessages();

    endResetModel();
}

uint64_t UnifiedTraceViewModel::makeDeltaKey(const BusMessage &msg)
{
    // Bits 33+: interface ID; bit 32: RX flag; bit 31: extended flag;
    // bits 29-30: bus type; bits 0-28: frame ID
    return static_cast<uint64_t>(msg.getInterfaceId()) << 33
         | static_cast<uint64_t>(msg.isRX()) << 32
         | static_cast<uint64_t>(msg.isExtended()) << 31
         | static_cast<uint64_t>(static_cast<uint8_t>(msg.busType())) << 29
         | (static_cast<uint64_t>(msg.getRawId()) & 0x1FFFFFFFull);
}

uint64_t UnifiedTraceViewModel::makeDeltaKey(const ProtocolMessage &pmsg)
{
    if (pmsg.rawFrames.isEmpty()) { return 0; }
    return makeDeltaKey(pmsg.rawFrames.first());
}

uint64_t UnifiedTraceViewModel::getJ1939Key(const ProtocolMessage& pmsg) const
{
    uint64_t ifId = pmsg.rawFrames.isEmpty() ? 0 : pmsg.rawFrames.first().getInterfaceId();
    uint32_t pgn = pmsg.id;
    uint32_t sa = pmsg.metadata.value("Source Address").toUInt();
    return (ifId << 32) | (pgn << 8) | (sa & 0xFF);
}

uint64_t UnifiedTraceViewModel::getUdsKey(const ProtocolMessage& pmsg) const
{
    uint64_t ifId = pmsg.rawFrames.isEmpty() ? 0 : pmsg.rawFrames.first().getInterfaceId();
    uint32_t canId = pmsg.rawFrames.isEmpty() ? 0 : pmsg.rawFrames.first().getRawId();
    return (ifId << 48) | (static_cast<uint64_t>(canId) << 16) | (pmsg.id & 0xFFFF);
}

QVariant UnifiedTraceViewModel::data_DisplayRole(const QModelIndex &index, [[maybe_unused]] int role) const
{
    UnifiedTraceItem *item = static_cast<UnifiedTraceItem*>(index.internalPointer());

    uint64_t current = item->timestamp();
    if (index.column() == column_index) {
        return (item->parentItem() == m_rootItem.get()) ? QVariant(item->globalIndex()) : QVariant();
    }

    if (item->isProtocol()) {
        const ProtocolMessage& pmsg = item->protocolMessage();
        switch (index.column()) {
            case column_timestamp:
            {
                return formatUnifiedTimestamp(current, item->prevSameIdTimestamp());
            }
            case column_canid:
            {
                uint32_t rawId = pmsg.rawFrames.isEmpty() ? 0 : pmsg.rawFrames.first().getId();
                QString rawStr = QString("0x%1").arg(rawId, 0, 16).toUpper();
                if (pmsg.protocol.compare("uds", Qt::CaseInsensitive) == 0) return QString("%1 (SID:%2)").arg(rawStr).arg(pmsg.id, 2, 16, QChar('0')).toUpper();
                if (pmsg.protocol.compare("j1939", Qt::CaseInsensitive) == 0) return QString("%1 (PGN:%2)").arg(rawStr).arg(pmsg.id, 0, 16).toUpper();
                return rawStr;
            }
            case column_type:
                if (pmsg.protocol.compare("j1939", Qt::CaseInsensitive) == 0) {
                    uint8_t pf = (pmsg.id >> 8) & 0xFF; // pmsg.id is PGN
                    return (pf < 240) ? "PDU1" : "PDU2";
                }
                return pmsg.protocol.toUpper();
            case column_name: return pmsg.name;
            case column_comment: return pmsg.description;
            case column_data:
                return BusMessage::formatDataBytes(pmsg.payload, dataAsciiMode());
            case column_dlc: return pmsg.payload.size();
            case column_direction: return pmsg.rawFrames.isEmpty() ? "" : (pmsg.rawFrames.first().isRX() ? tr("RX") : tr("TX"));
            case column_channel: return pmsg.rawFrames.isEmpty() ? "" : interfaceName(pmsg.rawFrames.first().getInterfaceId());
            case column_sender:
                if (pmsg.protocol.compare("uds", Qt::CaseInsensitive) == 0) {
                    return (pmsg.type == MessageType::Request) ? tr("Tester") : tr("ECU");
                }
                return "";
            default: return QVariant();
        }
    } else if (item->isSignal()) {
        UnifiedTraceItem *parent = item->parentItem();
        if (!parent) return QVariant();
        if (index.column() == column_index) return QVariant();
        return data_DisplayRole_Signal(index, role, parent->rawFrame());
    } else if (item->isMetadata()) {
        switch (index.column()) {
            case column_timestamp:
            {
                // Metadata rows are children — show delta relative to parent
                uint64_t prev = (item->parentItem() != m_rootItem.get()) ? item->parentItem()->timestamp() : 0;
                return formatUnifiedTimestamp(current, prev);
            }
            case column_name: return item->metadataName();
            case column_data: return item->metadataValue();
            case column_type:
                if (item->metadataName() == "Priority") return "P";
                if (item->metadataName() == "Reserved") return "R";
                if (item->metadataName() == "Data Page") return "DP";
                if (item->metadataName() == "PDU Format") return "PF";
                if (item->metadataName() == "PDU Specific") return "PS";
                if (item->metadataName() == "Source Address") return "SA";
                return "";
            default: return QVariant();
        }
    } else {
        const BusMessage& msg = item->rawFrame();
        switch (index.column()) {
            case column_index:
                return (item->parentItem() == m_rootItem.get()) ? QVariant(item->globalIndex()) : QVariant();
            case column_timestamp:
            {
                uint64_t prev;
                if (item->parentItem() == m_rootItem.get()) {
                    prev = item->prevSameIdTimestamp();
                } else {
                    prev = item->parentItem()->timestamp();
                }
                return formatUnifiedTimestamp(current, prev);
            }
            case column_channel: return interfaceName(msg.getInterfaceId());
            case column_direction: return msg.isRX() ? tr("RX") : tr("TX");
            case column_type: {
                if (msg.busType() == BusType::LIN) {
                    if (msg.isLinSleepFrame())  return QStringLiteral("LIN.SLP");
                    if (msg.isLinWakeupFrame()) return QStringLiteral("LIN.WUP");
                    return QStringLiteral("LIN");
                }
                if (msg.isErrorFrame()) return QStringLiteral("ERR");
                QString t;
                if (msg.isFD())       t += QStringLiteral("FD.");
                if (msg.isExtended()) t += QStringLiteral("EXT"); else t += QStringLiteral("STD");
                if (msg.isRTR())      t += QStringLiteral(".RTR");
                if (msg.isBRS())      t += QStringLiteral(".BRS");
                return t;
            }
            case column_canid:
                // A CAN error frame carries error classes instead of an ID; a LIN
                // frame flagged as an error is a real frame, so keep its ID.
                if (msg.isErrorFrame() && msg.busType() == BusType::CAN)
                    return QStringLiteral("-");
                return QString("0x%1").arg(msg.getId(), 0, 16).toUpper();
            case column_dlc: return msg.getLength();
            case column_data: return dataAsciiMode() ? msg.getDataAsciiString() : msg.getDataHexString();
            case column_name:
                if (msg.busType() == BusType::LIN) {
                    if (msg.isLinSleepFrame())  return tr("Sleep");
                    if (msg.isLinWakeupFrame()) return tr("Wakeup");
                    LinFrame *linFrame = backend()->findLinFrame(msg);
                    return linFrame ? linFrame->name() : QStringLiteral("");
                }
                if (item->parentItem() != m_rootItem.get()) {
                    uint8_t firstByte = msg.getByte(0);
                    uint8_t type = (firstByte >> 4) & 0x0F;
                    if (type == 0x0) return tr("[tp] Single Frame");
                    if (type == 0x1) return tr("[tp] First Frame");
                    if (type == 0x2) return tr("[tp] Consecutive Frame (SN: %1)").arg(firstByte & 0x0F);
                    if (type == 0x3) return tr("[tp] Flow Control");
                }
                {
                    CanDbMessage *dbmsg = backend()->findDbMessage(msg);
                    return (dbmsg) ? dbmsg->getName() : tr("[raw]");
                }
            case column_comment: {
                    CanDbMessage *dbmsg = backend()->findDbMessage(msg);
                    return (dbmsg) ? dbmsg->getComment() : "";
                }
            case column_sender: {
                if (msg.busType() == BusType::LIN) {
                    LinFrame *linFrame = backend()->findLinFrame(msg);
                    return linFrame ? linFrame->publisher() : QStringLiteral("");
                }
                return "";
            }
            default: return QVariant();
        }
    }
}

QVariant UnifiedTraceViewModel::data_TextColorRole(const QModelIndex &index, [[maybe_unused]] int role) const
{
    UnifiedTraceItem *item = static_cast<UnifiedTraceItem*>(index.internalPointer());
    bool isDark = ThemeManager::instance().isDarkMode();

    if (item->isProtocol()) {
        const ProtocolMessage& pmsg = item->protocolMessage();
        switch (pmsg.type) {
            case MessageType::Request:
                return isDark ? QColor(100, 180, 255) : QColor(0, 0, 139);
            case MessageType::PositiveResponse:
                return isDark ? QColor(120, 255, 120) : QColor(0, 100, 0);
            case MessageType::NegativeResponse:
                return isDark ? QColor(255, 120, 120) : QColor(139, 0, 0);
            default: break;
        }
    }
    const BusMessage& msg = item->rawFrame();
    if (msg.isErrorFrame()) return isDark ? QColor(255, 100, 100) : QColor(Qt::red);
    return QVariant();
}

QString UnifiedTraceViewModel::formatUnifiedTimestamp(uint64_t ts, uint64_t prevTs) const
{
    double val = 0;
    switch (timestampMode()) {
        case timestamp_mode_absolute:
            return QLocale::c().toString(
                QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts / 1000), Qt::LocalTime),
                "hh:mm:ss.zzz");
        case timestamp_mode_absolute_utc:
            return QLocale::c().toString(
                QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts / 1000), Qt::UTC),
                "hh:mm:ss.zzz");
        case timestamp_mode_relative:
        {
            const uint64_t startTs = static_cast<uint64_t>(backend()->getTimestampAtMeasurementStart() * 1000000.0);
            val = (ts >= startTs) ? static_cast<double>(ts - startTs) / 1000000.0 : 0.0;
            break;
        }
        case timestamp_mode_delta:
            val = (prevTs > 0 && ts >= prevTs) ? static_cast<double>(ts - prevTs) / 1000000.0 : 0.0;
            break;
        default:
            return QStringLiteral("0.000");
    }
    return QString::number(val, 'f', 3);
}
