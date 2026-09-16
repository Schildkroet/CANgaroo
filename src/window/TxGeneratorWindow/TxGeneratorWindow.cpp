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

#include "TxGeneratorWindow.h"
#include "ui_TxGeneratorWindow.h"

#include <algorithm>
#include <chrono>
#include <optional>

#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QTimer>
#include <QDialog>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QComboBox>
#include "core/Backend.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"
#include "driver/CanDriver.h"
#include "window/RawTxWindow/RawTxWindow.h"
#include <chrono>

TxGeneratorWindow::TxGeneratorWindow(QWidget *parent, Backend &backend) :
    ConfigurableWidget(parent),
    ui(new Ui::TxGeneratorWindow),
    _backend(backend)
{
    ui->setupUi(this);

    // Allow tree widgets to be compressed to near-zero so the window can be
    // made as short as the user wants (otherwise QTreeWidget's inherent
    // minimumSizeHint blocks shrinkage even after the window minimum is relaxed).
    ui->treeAvailable->setMinimumHeight(0);
    ui->treeActive->setMinimumHeight(0);

    // Deadline-driven scheduler: instead of polling, the timer is re-armed after
    // every pass for the earliest pending send deadline. That wakes the event loop
    // once per due frame (10x/s at a 100 ms cycle instead of 250x/s) and removes
    // the tick quantization that used to jitter the cycle time by up to one tick.
    // PreciseTimer is mandatory here -- the default CoarseTimerType grants itself
    // 5% slop, which would be +-5 ms at a 100 ms interval.
    _sendTimer = new QTimer(this);
    _sendTimer->setSingleShot(true);
    _sendTimer->setTimerType(Qt::PreciseTimer);
    connect(_sendTimer, &QTimer::timeout, this, &TxGeneratorWindow::onSendTimerTimeout);

    connect(&backend, &Backend::onSetupChanged, this, &TxGeneratorWindow::onSetupChanged);
    connect(&backend, &Backend::beginMeasurement, this, &TxGeneratorWindow::refreshInterfaces);
    connect(&backend, &Backend::beginMeasurement, this, &TxGeneratorWindow::updateMeasurementState);
    connect(&backend, &Backend::endMeasurement, this, &TxGeneratorWindow::refreshInterfaces);
    connect(&backend, &Backend::endMeasurement, this, &TxGeneratorWindow::updateMeasurementState);

    connect(ui->btnBulkRun, &QPushButton::clicked, this, &TxGeneratorWindow::on_btnBulkRun_clicked);
    connect(ui->btnBulkStop, &QPushButton::clicked, this, &TxGeneratorWindow::on_btnBulkStop_clicked);

    // Initial styling
    ui->btnBulkRun->setStyleSheet("QPushButton { font-weight: bold; background: #218838; color: white; border-radius: 4px; padding: 4px 8px; } QPushButton:hover { background: #28a745; } QPushButton:pressed { background: #196b2c; } QPushButton:disabled { background: #94d3a2; }");
    ui->btnBulkStop->setStyleSheet("QPushButton { font-weight: bold; background: #c82333; color: white; border-radius: 4px; padding: 4px 8px; } QPushButton:hover { background: #dc3545; } QPushButton:pressed { background: #a71d2a; } QPushButton:disabled { background: #f1aeb5; }");

    connect(ui->treeActive, &QTreeWidget::itemChanged, this, &TxGeneratorWindow::on_treeActive_itemChanged);
    connect(ui->treeActive, &QTreeWidget::itemDoubleClicked, this, &TxGeneratorWindow::treeActiveItemDoubleClicked);

    connect(ui->treeAvailable, &QTreeWidget::itemSelectionChanged, this, &TxGeneratorWindow::on_treeAvailable_itemSelectionChanged);

    _bitMatrixWidget = new BitMatrixWidget(this);
    // REMOVED redundant addWidget to verticalLayoutTabLayout

    // Configure Scroll Area
    ui->scrollAreaLayout->setWidgetResizable(false);
    ui->scrollAreaLayout->setWidget(_bitMatrixWidget);

    // Initialize Layout View controls
    ui->sliderLayoutZoom->setRange(30, 120);
    ui->sliderLayoutZoom->setValue(50);
    _bitMatrixWidget->setCellSize(50);
    _bitMatrixWidget->setFixedSize(_bitMatrixWidget->sizeHint());

    ui->spinInterval->setSuffix(" ms");
    ui->spinInterval->setMinimumWidth(0);

    ui->lineManualId->setInputMask("");
    ui->lineManualId->setValidator(new QRegularExpressionValidator(QRegularExpression("^[0-9A-Fa-f]{0,8}$"), this));
    ui->lineManualId->setText("001");
    connect(ui->lineManualId, &QLineEdit::textChanged, this, [this](const QString &text){
        if (text != text.toUpper()) {
            int cursorPos = ui->lineManualId->cursorPosition();
            ui->lineManualId->setText(text.toUpper());
            ui->lineManualId->setCursorPosition(cursorPos);
        }
    });

    ui->treeActive->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeAvailable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->btnAddToList->setEnabled(false);

    // Fit columns to content; let Name column stretch
    ui->treeActive->header()->setSectionResizeMode(0, QHeaderView::Fixed); // Status
    ui->treeActive->header()->resizeSection(0, 75);
    ui->treeActive->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents); // ID
    ui->treeActive->header()->setSectionResizeMode(2, QHeaderView::Stretch);           // Name
    ui->treeActive->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents); // Interface
    ui->treeActive->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents); // DLC
    ui->treeActive->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents); // Interval

    // Add Random Payload button programmatically
    _btnRandomPayload = new QPushButton(tr("Randomize Data"), this);
    _btnRandomPayload->setToolTip(tr("Randomize data bytes for selected messages"));
    _btnRandomPayload->setStyleSheet("QPushButton { font-weight: bold; background: #6f42c1; color: white; border-radius: 4px; padding: 4px 8px; } QPushButton:hover { background: #5a32a3; } QPushButton:pressed { background: #4a2d87; } QPushButton:disabled { background: #b8a9d4; }");
    ui->horizontalLayoutActiveControls->insertWidget(2, _btnRandomPayload); // Insert next to Run/Stop
    connect(_btnRandomPayload, &QPushButton::released, this, &TxGeneratorWindow::onRandomPayloadReleased);

    srand(time(nullptr));

    refreshInterfaces();
    updateMeasurementState();
    populateDbcMessages();
    updateActiveList();
    isLoading = false;
}

TxGeneratorWindow::~TxGeneratorWindow()
{
    delete ui;
}

void TxGeneratorWindow::retranslateUi()
{
    ui->retranslateUi(this);
}

bool TxGeneratorWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, xml, root)) { return false; }
    root.setAttribute("type", "TxGeneratorWindow");
    root.setAttribute("splitterState", QString(ui->splitter->saveState().toBase64()));

    for (const CyclicMessage &cm : _cyclicMessages)
    {
        // Resolve the display name of the selected interface from the combo box.
        QString ifName;
        for (int i = 0; i < ui->comboBoxInterface->count(); i++)
        {
            if (static_cast<BusInterfaceId>(ui->comboBoxInterface->itemData(i).toUInt()) == cm.interfaceId)
            {
                ifName = ui->comboBoxInterface->itemText(i);
                break;
            }
        }

        // Encode payload as space-separated uppercase hex bytes.
        QString dataStr;
        for (int i = 0; i < cm.msg.getLength(); i++)
        {
            if (i > 0) { dataStr += ' '; }
            dataStr += QString("%1").arg(cm.msg.getByte(i), 2, 16, QChar('0')).toUpper();
        }

        QDomElement frameEl = xml.createElement("frame");
        frameEl.setAttribute("id",       QString("0x%1").arg(cm.msg.getId(), 0, 16).toUpper());
        frameEl.setAttribute("name",     cm.name);
        frameEl.setAttribute("dlc",      cm.msg.getLength());
        frameEl.setAttribute("extended", cm.msg.isExtended() ? 1 : 0);
        frameEl.setAttribute("fd",       cm.msg.isFD()       ? 1 : 0);
        frameEl.setAttribute("brs",      cm.msg.isBRS()      ? 1 : 0);
        frameEl.setAttribute("interval", cm.interval);
        frameEl.setAttribute("interface", ifName);
        frameEl.setAttribute("data",     dataStr);
        root.appendChild(frameEl);
    }

    return true;
}

bool TxGeneratorWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el)) { return false; }

    QString splitterState = el.attribute("splitterState");
    if (!splitterState.isEmpty()) {
        ui->splitter->restoreState(QByteArray::fromBase64(splitterState.toLatin1()));
    }

    _cyclicMessages.clear();

    QDomNodeList frames = el.elementsByTagName("frame");
    for (int i = 0; i < frames.count(); i++)
    {
        QDomElement frameEl = frames.at(i).toElement();

        CyclicMessage cm;
        cm.msg.setId(frameEl.attribute("id").toUInt(nullptr, 16));
        const int dlc = frameEl.attribute("dlc").toInt();
        cm.msg.setLength(static_cast<uint8_t>(dlc));
        cm.msg.setExtended(frameEl.attribute("extended").toInt() != 0);
        cm.msg.setFD(      frameEl.attribute("fd").toInt()       != 0);
        cm.msg.setBRS(     frameEl.attribute("brs").toInt()      != 0);
        cm.name          = frameEl.attribute("name");
        cm.interval      = frameEl.attribute("interval", "100").toInt();
        cm.enabled       = false; // Never auto-start on load
        cm.nextDue       = {};
        cm.interfaceId   = 0;
        cm.dbMsg         = nullptr;
        cm.interfaceName = frameEl.attribute("interface");

        // Decode space-separated hex payload.
        const QStringList bytes = frameEl.attribute("data").split(' ', Qt::SkipEmptyParts);
        for (int j = 0; j < bytes.size() && j < dlc; j++)
        {
            cm.msg.setDataAt(static_cast<uint8_t>(j),
                             static_cast<uint8_t>(bytes[j].toUInt(nullptr, 16)));
        }

        _cyclicMessages.append(cm);
    }

    // Interface IDs are runtime-assigned; try to resolve them now (may be a
    // no-op if the setup hasn't been loaded yet — resolveInterfaceNames() is
    // also called from refreshInterfaces() once the setup is applied).
    resolveInterfaceNames();
    // dbMsg is a runtime pointer and can't be persisted; re-link by raw CAN ID
    // now (may be a no-op if the setup/DBCs haven't been loaded yet —
    // resolveDbMessages() is also called from refreshInterfaces()).
    resolveDbMessages();
    updateActiveList();
    return true;
}

void TxGeneratorWindow::resolveInterfaceNames()
{
    for (CyclicMessage &cm : _cyclicMessages)
    {
        if (cm.interfaceName.isEmpty()) { continue; }
        for (int i = 0; i < ui->comboBoxInterface->count(); i++)
        {
            if (ui->comboBoxInterface->itemText(i) == cm.interfaceName)
            {
                cm.interfaceId = static_cast<BusInterfaceId>(ui->comboBoxInterface->itemData(i).toUInt());
                break;
            }
        }
    }
}

void TxGeneratorWindow::resolveDbMessages()
{
    MeasurementSetup &setup = _backend.getSetup();
    for (CyclicMessage &cm : _cyclicMessages)
    {
        if (cm.dbMsg) { continue; }
        cm.dbMsg = setup.findDbMessage(cm.msg);
    }
}

void TxGeneratorWindow::refreshInterfaces()
{
    ui->comboBoxInterface->blockSignals(true);
    ui->comboBoxInterface->clear();

    MeasurementSetup &setup = _backend.getSetup();
    for (auto *network : setup.getNetworks()) {
        for (auto *mi : network->interfaces()) {
            BusInterfaceId ifid = mi->busInterface();
            BusInterface *intf = _backend.getInterfaceById(ifid);
            if (intf) {
                QString name = network->name() + ": " + intf->getName();
                ui->comboBoxInterface->addItem(name, QVariant(ifid));
            }
        }
    }
    if (ui->comboBoxInterface->count() > 0 && ui->comboBoxInterface->currentIndex() == -1) {
        ui->comboBoxInterface->setCurrentIndex(0);
    }
    ui->comboBoxInterface->blockSignals(false);

    // Re-resolve interface names in case frames were loaded before the setup existed.
    resolveInterfaceNames();
    // Re-resolve dbMsg links in case frames were loaded before the DBCs existed.
    resolveDbMessages();
    populateDbcMessages();
}

void TxGeneratorWindow::populateDbcMessages()
{
    ui->treeAvailable->clear();

    BusInterfaceId currentId = static_cast<BusInterfaceId>(ui->comboBoxInterface->currentData().toUInt());
    MeasurementSetup &setup = _backend.getSetup();

    for (auto *network : setup.getNetworks()) {
        // Only show DBCs associated with the current interface if possible,
        // but currently networks map to interfaces.
        // Let's find if this network is using our interface.
        bool interfaceMatches = false;
        for (auto *mi : network->interfaces()) {
            if (mi->busInterface() == currentId) {
                interfaceMatches = true;
                break;
            }
        }

        if (interfaceMatches) {
            for (const auto &db : network->_canDbs) {
                if (db) {
                    CanDbMessageList msgs = db->getMessageList();
                    for (auto it = msgs.begin(); it != msgs.end(); ++it) {
                        CanDbMessage *dbMsg = *it;
                        if (dbMsg) {
                            QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeAvailable);
                            item->setText(0, "0x" + QString("%1").arg(dbMsg->getRaw_id(), 3, 16, QChar('0')).toUpper());
                            item->setText(1, dbMsg->getName());
                            item->setData(0, Qt::UserRole, QVariant::fromValue((void*)dbMsg));
                        }
                    }
                }
            }
        }
    }
}

void TxGeneratorWindow::on_lineEditSearchAvailable_textChanged(const QString &text)
{
    for (int i = 0; i < ui->treeAvailable->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = ui->treeAvailable->topLevelItem(i);
        bool match = item->text(0).contains(text, Qt::CaseInsensitive) || item->text(1).contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

void TxGeneratorWindow::on_treeAvailable_itemSelectionChanged()
{
    ui->btnAddToList->setEnabled(!ui->treeAvailable->selectedItems().isEmpty());

    QTreeWidgetItem *item = ui->treeAvailable->currentItem();
    if (item && _bitMatrixWidget) {
        CanDbMessage *dbMsg = static_cast<CanDbMessage*>(item->data(0, Qt::UserRole).value<void*>());
        _bitMatrixWidget->setMessage(dbMsg);
    } else if (_bitMatrixWidget) {
        _bitMatrixWidget->setMessage(nullptr);
    }
}

void TxGeneratorWindow::on_sliderLayoutZoom_valueChanged(int value)
{
    if (_bitMatrixWidget) {
        _bitMatrixWidget->setCellSize(value);
        _bitMatrixWidget->setFixedSize(_bitMatrixWidget->sizeHint());
    }
}

void TxGeneratorWindow::on_cbLayoutCompact_toggled(bool checked)
{
    if (_bitMatrixWidget) {
        _bitMatrixWidget->setCompactMode(checked);
        _bitMatrixWidget->setFixedSize(_bitMatrixWidget->sizeHint());
    }
}

void TxGeneratorWindow::on_btnAddToList_released()
{
    QList<QTreeWidgetItem*> selected = ui->treeAvailable->selectedItems();
    if (selected.isEmpty()) {
        QTreeWidgetItem *current = ui->treeAvailable->currentItem();
        if (current) selected.append(current);
    }

    if (selected.isEmpty()) return;

    for (auto *item : selected) {
        CanDbMessage *dbMsg = static_cast<CanDbMessage*>(item->data(0, Qt::UserRole).value<void*>());
        if (!dbMsg) continue;

        CyclicMessage cm;
        cm.msg = BusMessage(); // Ensure fresh instance
        cm.msg.setId(dbMsg->getRaw_id());
        cm.msg.setLength(dbMsg->getDlc());
        cm.msg.setExtended(dbMsg->getRaw_id() > 0x7FF);
        cm.name = dbMsg->getName();
        cm.interval = 100;
        cm.enabled = false;
        cm.nextDue = {};
        cm.interfaceId = static_cast<BusInterfaceId>(ui->comboBoxInterface->currentData().toUInt());
        cm.dbMsg = dbMsg;

        _cyclicMessages.append(cm);
    }
    updateActiveList();
    ui->treeActive->scrollToBottom();
}

void TxGeneratorWindow::on_btnAddManual_released()
{
    bool ok;
    uint32_t id = ui->lineManualId->text().toUInt(&ok, 16);
    if (!ok) return;

    CyclicMessage cm;
    cm.msg = BusMessage(); // Ensure fresh instance
    cm.msg.setId(id);
    cm.msg.setLength(ui->spinManualDlc->value());
    cm.msg.setExtended(id > 0x7FF || ui->lineManualId->text().length() > 3);
    cm.msg.setFD(ui->spinManualDlc->value() > 8);
    cm.name = "Manual";
    cm.interval = ui->spinInterval->value();
    cm.enabled = false;
    cm.nextDue = {};
    cm.interfaceId = static_cast<BusInterfaceId>(ui->comboBoxInterface->currentData().toUInt());
    cm.dbMsg = nullptr;

    _cyclicMessages.append(cm);
    updateActiveList();
    ui->treeActive->scrollToBottom();
}

void TxGeneratorWindow::on_btnRemove_released()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) return;

    // To avoid index shifting issues, we collect rows and remove from highest to lowest
    QList<int> rows;
    for (auto *item : selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0) rows.append(row);
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    for (auto row : rows) {
        if (row >= 0 && row < _cyclicMessages.size()) {
            _cyclicMessages.removeAt(row);
        }
    }
    updateActiveList();
}

void TxGeneratorWindow::on_btnSendOnce_released()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            selected.append(ui->treeActive->currentItem());
        }
    }

    for (auto *item : selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0 && row < _cyclicMessages.size()) {
            CyclicMessage &cm = _cyclicMessages[row];
            BusInterface *intf = _backend.getInterfaceById(cm.interfaceId);
            if (intf && intf->isOpen()) {
                cm.msg.setInterfaceId(cm.interfaceId);
                intf->sendMessage(cm.msg);
                /*BusMessage loopback = cm.msg;
                loopback.setRX(false);
                auto now = std::chrono::system_clock::now().time_since_epoch();
                loopback.setTimestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
                emit loopbackFrame(loopback);*/
            } else {
                QString errorMsg = QString("TxGeneratorWindow: Interface %1 is not open.").arg(intf ? intf->getName() : QString::number(cm.interfaceId));
                log_error(errorMsg);
            }
        }
    }
}

void TxGeneratorWindow::on_btnBulkRun_clicked()
{
    if (!_backend.isMeasurementRunning()) {
        ui->btnBulkRun->setChecked(false);
        return;
    }

    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) return;

    for (auto *item : selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0 && row < _cyclicMessages.size()) {
            setEnabled(row, true);
        }
    }

    ui->btnBulkRun->setChecked(true);
    ui->btnBulkStop->setChecked(false);
    updateSendTimer();
}

void TxGeneratorWindow::on_btnBulkStop_clicked()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        // Fallback to active index if nothing selected
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            setEnabled(row, false);
        }
    } else {
        for (auto *item : selected) {
            int row = ui->treeActive->indexOfTopLevelItem(item);
            if (row >= 0 && row < _cyclicMessages.size()) {
                _cyclicMessages[row].enabled = false;
                updateRowUI(row);
            }
        }
    }

    ui->btnBulkRun->setChecked(false);
    ui->btnBulkStop->setChecked(true);
    updateSendTimer();
}

void TxGeneratorWindow::on_spinInterval_valueChanged(int i)
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            setInterval(row, i);
        }
    } else {
        for (auto *item : selected) {
            int row = ui->treeActive->indexOfTopLevelItem(item);
            if (row >= 0 && row < _cyclicMessages.size()) {
                setInterval(row, i);
            }
        }
    }
    updateSendTimer();
}

void TxGeneratorWindow::on_comboBoxInterface_currentIndexChanged(int index)
{
    (void)index;
    populateDbcMessages();
    emit interfaceChanged(static_cast<BusInterfaceId>(ui->comboBoxInterface->currentData().toUInt()));
}

void TxGeneratorWindow::on_treeAvailable_itemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(item);
    Q_UNUSED(column);
    on_btnAddToList_released();
}

void TxGeneratorWindow::on_treeActive_itemSelectionChanged()
{
    isLoading = true;
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (!selected.isEmpty()) {
        int row = ui->treeActive->indexOfTopLevelItem(selected.first());
        if (row >= 0 && row < _cyclicMessages.size()) {
            const CyclicMessage &cm = _cyclicMessages[row];

            ui->btnBulkRun->blockSignals(true);
            ui->btnBulkStop->blockSignals(true);
            ui->btnBulkRun->setChecked(cm.enabled);
            ui->btnBulkStop->setChecked(!cm.enabled);
            ui->btnBulkRun->blockSignals(false);
            ui->btnBulkStop->blockSignals(false);

            ui->spinInterval->blockSignals(true);
            ui->spinInterval->setValue(cm.interval);
            ui->spinInterval->blockSignals(false);


        }
    } else {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            const CyclicMessage &cm = _cyclicMessages[row];
            ui->btnBulkRun->blockSignals(true);
            ui->btnBulkStop->blockSignals(true);
            ui->btnBulkRun->setChecked(cm.enabled);
            ui->btnBulkStop->setChecked(!cm.enabled);
            ui->btnBulkRun->blockSignals(false);
            ui->btnBulkStop->blockSignals(false);

            ui->spinInterval->blockSignals(true);
            ui->spinInterval->setValue(cm.interval);
            ui->spinInterval->blockSignals(false);


        }
    }
    isLoading = false;
}

void TxGeneratorWindow::treeActiveItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    int row = ui->treeActive->indexOfTopLevelItem(item);
    if (row < 0 || row >= _cyclicMessages.size()) { return; }

    CyclicMessage &cm = _cyclicMessages[row];

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit Message: %1 - %2").arg(item->text(1), cm.name));
    dlg.resize(750, 480);

    auto *layout = new QVBoxLayout(&dlg);
    auto *rawTx = new RawTxWindow(&dlg, _backend);
    rawTx->setMessage(cm.msg, cm.name, cm.interfaceId, cm.dbMsg);
    layout->addWidget(rawTx);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    BusMessage editedMsg = cm.msg;
    BusInterfaceId editedInterfaceId = cm.interfaceId;
    QString editedInterfaceName = cm.interfaceName;

    connect(rawTx, &RawTxWindow::messageUpdated, this, [&editedMsg](const BusMessage &msg) {
        editedMsg = msg;
    });
    connect(rawTx, &RawTxWindow::interfaceSelected, this, [&](BusInterfaceId id) {
        editedInterfaceId = id;
        for (int i = 0; i < ui->comboBoxInterface->count(); ++i) {
            if (static_cast<BusInterfaceId>(ui->comboBoxInterface->itemData(i).toUInt()) == id) {
                editedInterfaceName = ui->comboBoxInterface->itemText(i);
                break;
            }
        }
    });

    if (dlg.exec() == QDialog::Accepted)
    {
        cm.msg           = editedMsg;
        cm.interfaceId   = editedInterfaceId;
        cm.interfaceName = editedInterfaceName;
        updateRowUI(row);
    }
}

void TxGeneratorWindow::on_treeActive_itemChanged(QTreeWidgetItem *item, int column)
{
    int row = ui->treeActive->indexOfTopLevelItem(item);
    if (row >= 0 && row < _cyclicMessages.size()) {
        if (column == 5) {
            bool ok;
            int interval = item->text(5).toInt(&ok);
            if (ok && interval > 0 && _cyclicMessages[row].interval != interval) {
                setInterval(row, interval);

                // If this item is part of a selection, apply to all selected items
                if (item->isSelected()) {
                    for (auto *selItem : ui->treeActive->selectedItems()) {
                        if (selItem == item) continue;
                        int selRow = ui->treeActive->indexOfTopLevelItem(selItem);
                        if (selRow >= 0 && selRow < _cyclicMessages.size()) {
                            setInterval(selRow, interval);
                        }
                    }
                }
                updateSendTimer();
            }
        }
    }
}

void TxGeneratorWindow::on_btnSelectAll_released()
{
    ui->treeActive->selectAll();
}

void TxGeneratorWindow::on_btnClearAll_released()
{
    ui->treeActive->clearSelection();
}

void TxGeneratorWindow::onStatusButtonClicked()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    // Determine the row of the button
    QPoint pos = btn->parentWidget()->mapTo(ui->treeActive->viewport(), btn->pos());
    QTreeWidgetItem *item = ui->treeActive->itemAt(pos);
    if (!item) return;

    int row = ui->treeActive->indexOfTopLevelItem(item);
    if (row >= 0 && row < _cyclicMessages.size()) {
        bool targetState = !_cyclicMessages[row].enabled;

        // If this item is part of a selection, apply to all selected items
        if (item->isSelected()) {
            for (auto *selItem : ui->treeActive->selectedItems()) {
                int selRow = ui->treeActive->indexOfTopLevelItem(selItem);
                if (selRow >= 0 && selRow < _cyclicMessages.size()) {
                    setEnabled(selRow, targetState);
                }
            }
        } else {
            setEnabled(row, targetState);
        }
        updateSendTimer();
    }
}

void TxGeneratorWindow::onSendTimerTimeout()
{
    if (!_backend.isMeasurementRunning()) {
        _sendTimer->stop();
        return;
    }
    const auto now = TxClock::now();

    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        CyclicMessage &cm = _cyclicMessages[i];
        if (!cm.enabled) { continue; }

        const auto period = std::chrono::milliseconds(std::max(1, cm.interval));
        if (cm.nextDue == TxClock::time_point{}) { cm.nextDue = now; } // first send after enable
        if (cm.nextDue > now) { continue; }

        BusInterface *intf = _backend.getInterfaceById(cm.interfaceId);
        if (intf && intf->isOpen()) {
            cm.msg.setInterfaceId(cm.interfaceId);
            intf->sendMessage(cm.msg);

            // Advance from the scheduled deadline, never from the actual send
            // time, so event-loop latency cannot accumulate into cycle drift.
            cm.nextDue += period;
            if (cm.nextDue <= now) {
                // More than one period behind (event loop stalled, or the cycle
                // time was shortened): skip the missed slots instead of bursting.
                const auto missed = (now - cm.nextDue) / period + 1;
                cm.nextDue += period * missed;
            }
        } else {
            // Disable to avoid error spam; user must re-enable manually
            cm.enabled = false;
            updateRowUI(i);
            log_error(QString("TxGeneratorWindow: Interface %1 is not open.")
                .arg(intf ? intf->getName() : QString::number(cm.interfaceId)));
        }
    }
    updateSendTimer();
}

void TxGeneratorWindow::onSetupChanged()
{
    refreshInterfaces();
}

void TxGeneratorWindow::updateMeasurementState()
{
    // The active list stays editable without a measurement (add, remove, edit,
    // change interval); only actions that put frames on the bus need one.
    bool running = _backend.isMeasurementRunning();
    ui->btnSendOnce->setEnabled(running);
    ui->btnBulkRun->setEnabled(running);
    if (!running) {
        stopAll();
    }
    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        updateRowUI(i);
    }
    updateSendTimer();
}

void TxGeneratorWindow::updateActiveList()
{
    // Save selection
    QList<int> selectedRows;
    for (auto *item : ui->treeActive->selectedItems()) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0) selectedRows.append(row);
    }
    int currentRow = ui->treeActive->currentIndex().row();

    ui->treeActive->blockSignals(true);
    ui->treeActive->clear();
    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        const CyclicMessage &cm = _cyclicMessages[i];
        QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeActive);

        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable); // Explicitly remove checkbox

        // Create Status Button
        QPushButton *btnStatus = new QPushButton(cm.enabled ? "⏹" : "▶");
        btnStatus->setToolTip(cm.enabled ? "Stop" : "Start");
        btnStatus->setFixedWidth(40);
        if (cm.enabled) {
            btnStatus->setStyleSheet("QPushButton { color: #dc3545; font-weight: bold; background: transparent; border: 1px solid #dc3545; border-radius: 3px; } QPushButton:hover { background: #dc3545; color: white; }");
        } else {
            btnStatus->setStyleSheet("QPushButton { color: #28a745; font-weight: bold; background: transparent; border: 1px solid #28a745; border-radius: 3px; } QPushButton:hover { background: #28a745; color: white; }");
        }

        // Starting needs a running measurement; stopping is always allowed.
        btnStatus->setEnabled(cm.enabled || _backend.isMeasurementRunning());
        connect(btnStatus, &QPushButton::clicked, this, &TxGeneratorWindow::onStatusButtonClicked);
        ui->treeActive->setItemWidget(item, 0, btnStatus);

        item->setText(1, "0x" + QString("%1").arg(cm.msg.getId(), 3, 16, QChar('0')).toUpper());
        item->setText(2, cm.name);
        BusInterface *intf = _backend.getInterfaceById(cm.interfaceId);
        item->setText(3, intf ? intf->getName() : "Unknown");
        item->setText(4, QString::number(cm.msg.getLength()));
        item->setText(5, QString::number(cm.interval));

        if (selectedRows.contains(i)) {
            item->setSelected(true);
        }
        if (i == currentRow) {
            ui->treeActive->setCurrentItem(item);
        }
    }
    ui->treeActive->blockSignals(false);
}

void TxGeneratorWindow::updateRowUI(int row)
{
    if (row < 0 || row >= _cyclicMessages.size()) return;
    QTreeWidgetItem *item = ui->treeActive->topLevelItem(row);
    if (!item) return;

    const CyclicMessage &cm = _cyclicMessages[row];

    ui->treeActive->blockSignals(true);

    // Update button in column 0
    QPushButton *btnStatus = qobject_cast<QPushButton*>(ui->treeActive->itemWidget(item, 0));
    if (btnStatus) {
        btnStatus->setText(cm.enabled ? "⏹" : "▶");
        btnStatus->setToolTip(cm.enabled ? "Stop" : "Start");
        if (cm.enabled) {
            btnStatus->setStyleSheet("QPushButton { color: #dc3545; font-weight: bold; background: transparent; border: 1px solid #dc3545; border-radius: 3px; } QPushButton:hover { background: #dc3545; color: white; }");
        } else {
            btnStatus->setStyleSheet("QPushButton { color: #28a745; font-weight: bold; background: transparent; border: 1px solid #28a745; border-radius: 3px; } QPushButton:hover { background: #28a745; color: white; }");
        }
        btnStatus->setEnabled(cm.enabled || _backend.isMeasurementRunning());
    }

    item->setText(1, "0x" + QString("%1").arg(cm.msg.getId(), 3, 16, QChar('0')).toUpper());
    item->setText(2, cm.name);
    BusInterface *intf = _backend.getInterfaceById(cm.interfaceId);
    item->setText(3, intf ? intf->getName() : "Unknown");
    item->setText(4, QString::number(cm.msg.getLength()));
    item->setText(5, QString::number(cm.interval));

    ui->treeActive->blockSignals(false);
}


void TxGeneratorWindow::stopAll()
{
    for (int i = 0; i < _cyclicMessages.size(); ++i) {
        _cyclicMessages[i].enabled = false;
        updateRowUI(i);
    }
    ui->btnBulkRun->setChecked(false);
    ui->btnBulkStop->setChecked(false);
}

QSize TxGeneratorWindow::sizeHint() const
{
    return QSize(1200, 400);
}

void TxGeneratorWindow::setInterval(int row, int interval_ms)
{
    CyclicMessage &cm = _cyclicMessages[row];
    cm.interval = interval_ms;

    // A shorter cycle time must take effect now, not after the deadline that was
    // armed for the old (longer) one.
    const auto latest = TxClock::now() + std::chrono::milliseconds(std::max(1, interval_ms));
    if (cm.nextDue > latest) { cm.nextDue = latest; }

    updateRowUI(row);
}

void TxGeneratorWindow::setEnabled(int row, bool enabled)
{
    CyclicMessage &cm = _cyclicMessages[row];
    if (enabled && !_backend.isMeasurementRunning()) { return; } // nothing can be sent
    if (enabled && !cm.enabled) { cm.nextDue = {}; } // fresh schedule, send immediately
    cm.enabled = enabled;
    updateRowUI(row);
}

void TxGeneratorWindow::updateSendTimer()
{
    if (!_backend.isMeasurementRunning()) {
        _sendTimer->stop();
        return;
    }

    const auto now = TxClock::now();
    std::optional<TxClock::time_point> earliest;
    for (const CyclicMessage &cm : std::as_const(_cyclicMessages)) {
        if (!cm.enabled) { continue; }
        // An unscheduled message is due right away.
        const auto due = (cm.nextDue == TxClock::time_point{}) ? now : cm.nextDue;
        if (!earliest || due < *earliest) { earliest = due; }
    }

    if (!earliest) {
        _sendTimer->stop();
        return;
    }

    // Round up so the timer never fires before the deadline, which would only
    // cost an extra wakeup and re-arm.
    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(*earliest - now);
    _sendTimer->start(std::max<qint64>(0, delay.count()));
}

void TxGeneratorWindow::onRandomPayloadReleased()
{
    QList<QTreeWidgetItem*> selected = ui->treeActive->selectedItems();
    if (selected.isEmpty()) {
        int row = ui->treeActive->currentIndex().row();
        if (row >= 0 && row < _cyclicMessages.size()) {
            selected.append(ui->treeActive->currentItem());
        }
    }

    for (auto *item : selected) {
        int row = ui->treeActive->indexOfTopLevelItem(item);
        if (row >= 0 && row < _cyclicMessages.size()) {
            CyclicMessage &cm = _cyclicMessages[row];
            for (int i = 0; i < cm.msg.getLength(); ++i) {
                cm.msg.setDataAt(i, static_cast<uint8_t>(rand() % 256));
            }
            updateRowUI(row);

            // If this is the currently focused message in the bit matrix, update it
            if (item == ui->treeActive->currentItem()) {

            }
        }
    }
}

