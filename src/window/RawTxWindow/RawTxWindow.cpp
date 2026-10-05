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

#include "RawTxWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QRegularExpressionValidator>
#include <QSplitter>
#include <QDomDocument>
#include <QFont>

#include "core/Backend.h"
#include "db/model/CanDbMessage.h"
#include "db/model/CanDbSignal.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"

RawTxWindow::RawTxWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(backend)
    , _currentDbMsg(nullptr)
    , _slavedInterfaceId(0)
    , _settingMessage(false)
    , _updatingSignals(false)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    // --- Header: ID, DLC, flags ---
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(8);

    headerLayout->addWidget(new QLabel(tr("ID (Hex):"), this));
    _editId = new QLineEdit(this);
    _editId->setMaximumWidth(120);
    _editId->setValidator(new QRegularExpressionValidator(QRegularExpression("^[0-9A-Fa-f]{0,8}$"), this));
    _editId->setPlaceholderText("000");
    headerLayout->addWidget(_editId);

    headerLayout->addWidget(new QLabel(tr("DLC:"), this));
    _comboDlc = new QComboBox(this);
    _comboDlc->setMinimumWidth(60);
    headerLayout->addWidget(_comboDlc);

    _cbExtended = new QCheckBox(tr("Extended"), this);
    _cbRTR = new QCheckBox(tr("RTR"), this);
    _cbFD = new QCheckBox(tr("FD"), this);
    _cbBRS = new QCheckBox(tr("BRS"), this);
    _cbBRS->setEnabled(false);

    headerLayout->addWidget(_cbExtended);
    headerLayout->addWidget(_cbRTR);
    headerLayout->addWidget(_cbFD);
    headerLayout->addWidget(_cbBRS);
    headerLayout->addStretch();

    headerLayout->addWidget(new QLabel(tr("Interface:"), this));
    _comboInterface = new QComboBox(this);
    _comboInterface->setMinimumWidth(180);
    headerLayout->addWidget(_comboInterface);

    connect(_comboInterface, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        BusInterfaceId id = static_cast<BusInterfaceId>(_comboInterface->currentData().toUInt());
        _slavedInterfaceId = id;
        emit interfaceSelected(id);
    });

    mainLayout->addLayout(headerLayout);

    // --- Data hex grid ---
    _dataTable = new QTableWidget(MaxDataRows, DataCols, this);
    _dataTable->horizontalHeader()->setDefaultSectionSize(36);
    _dataTable->verticalHeader()->setDefaultSectionSize(26);
    _dataTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    _dataTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    _dataTable->setSelectionMode(QAbstractItemView::SingleSelection);

    QFont mono("Monospace");
    mono.setStyleHint(QFont::TypeWriter);
    _dataTable->setFont(mono);

    // Column headers: byte offsets 0-7
    QStringList colHeaders;
    for (int c = 0; c < DataCols; c++) {
        colHeaders << QString::number(c);
    }
    _dataTable->setHorizontalHeaderLabels(colHeaders);

    // Row headers: offset labels (0, 8, 16, ...)
    QStringList rowHeaders;
    for (int r = 0; r < MaxDataRows; r++) {
        rowHeaders << QString::number(r * DataCols);
    }
    _dataTable->setVerticalHeaderLabels(rowHeaders);

    // Populate cells with "00"
    for (int r = 0; r < MaxDataRows; r++) {
        for (int c = 0; c < DataCols; c++) {
            auto *item = new QTableWidgetItem("00");
            item->setTextAlignment(Qt::AlignCenter);
            _dataTable->setItem(r, c, item);
        }
    }

    // Size the table to fit contents
    int tableHeight = _dataTable->horizontalHeader()->height()
                    + _dataTable->verticalHeader()->defaultSectionSize() + 2;
    _dataTable->setMinimumHeight(tableHeight);
    _dataTable->setMaximumHeight(tableHeight * MaxDataRows);

    // --- Signal table ---
    _signalTable = new QTableWidget(0, 3, this);
    _signalTable->setHorizontalHeaderLabels({tr("Signal"), tr("Value"), tr("Unit")});
    _signalTable->horizontalHeader()->setStretchLastSection(true);
    _signalTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _signalTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::AnyKeyPressed);
    _signalTable->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(_dataTable);
    splitter->addWidget(_signalTable);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter);

    // --- Populate DLC combo (classic CAN by default) ---
    populateDlcCombo(false);

    // --- Connections ---
    connect(_editId, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text != text.toUpper()) {
            int pos = _editId->cursorPosition();
            _editId->setText(text.toUpper());
            _editId->setCursorPosition(pos);
        }
        // Auto-set extended for IDs > 0x7FF
        if (!_settingMessage) {
            uint32_t id = _editId->text().toUInt(nullptr, 16);
            if (id > 0x7FF || text.length() > 3) {
                _cbExtended->setChecked(true);
            }
        }
        onFieldChanged();
    });

    connect(_comboDlc, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateDataGrid();
        onFieldChanged();
    });

    connect(_cbExtended, &QCheckBox::stateChanged, this, &RawTxWindow::onFieldChanged);
    connect(_cbRTR, &QCheckBox::stateChanged, this, &RawTxWindow::onFieldChanged);
    connect(_cbFD, &QCheckBox::stateChanged, this, [this]() {
        bool fd = _cbFD->isChecked();
        _cbBRS->setEnabled(fd);
        if (!fd) { _cbBRS->setChecked(false); }
        // RTR not allowed with FD
        _cbRTR->setEnabled(!fd);
        if (fd) { _cbRTR->setChecked(false); }
        // Force DLC > 8 to enable FD
        int dlc = currentDlc();
        if (dlc > 8 && !fd) {
            _cbFD->setChecked(true);
            return;
        }
        updateDataGrid();
        onFieldChanged();
    });
    connect(_cbBRS, &QCheckBox::stateChanged, this, &RawTxWindow::onFieldChanged);

    connect(_dataTable, &QTableWidget::cellChanged, this, &RawTxWindow::onFieldChanged);
    connect(_signalTable, &QTableWidget::cellChanged, this, &RawTxWindow::onSignalValueChanged);

    // Initial state
    updateDataGrid();
    this->setEnabled(false);
}

void RawTxWindow::retranslateUi()
{
}

int RawTxWindow::currentDlc() const
{
    return _comboDlc->currentData().toInt();
}

void RawTxWindow::populateDlcCombo(bool canfd)
{
    int prevDlc = _comboDlc->currentData().toInt();
    _comboDlc->blockSignals(true);
    _comboDlc->clear();

    for (int d = 0; d <= 8; d++) {
        _comboDlc->addItem(QString::number(d), d);
    }
    if (canfd) {
        static const int fdSizes[] = {12, 16, 20, 24, 32, 48, 64};
        for (int s : fdSizes) {
            _comboDlc->addItem(QString::number(s), s);
        }
    }

    // Restore previous DLC if possible
    int idx = _comboDlc->findData(prevDlc);
    if (idx >= 0) {
        _comboDlc->setCurrentIndex(idx);
    }
    _comboDlc->blockSignals(false);
}

void RawTxWindow::updateDataGrid()
{
    int dlc = currentDlc();
    int rows = (dlc + DataCols - 1) / DataCols;
    if (rows < 1) { rows = 1; }

    // Show/hide rows
    for (int r = 0; r < MaxDataRows; r++) {
        _dataTable->setRowHidden(r, r >= rows);
    }

    // Enable/disable cells beyond DLC
    _dataTable->blockSignals(true);
    for (int r = 0; r < MaxDataRows; r++) {
        for (int c = 0; c < DataCols; c++) {
            int byteIdx = r * DataCols + c;
            QTableWidgetItem *item = _dataTable->item(r, c);
            if (item) {
                if (byteIdx < dlc) {
                    item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsEnabled);
                } else {
                    item->setFlags(item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsEnabled));
                }
            }
        }
    }
    _dataTable->blockSignals(false);

    // Resize table height to visible rows
    int headerH = _dataTable->horizontalHeader()->height();
    int rowH = _dataTable->verticalHeader()->defaultSectionSize();
    _dataTable->setFixedHeight(headerH + rowH * rows + 2);
}

void RawTxWindow::onFieldChanged()
{
    if (_settingMessage) { return; }

    uint32_t id = _editId->text().toUInt(nullptr, 16);
    if (id > 0x1FFFFFFF) {
        id = 0x1FFFFFFF;
    }

    int dlc = currentDlc();
    bool fd = _cbFD->isChecked();
    if (dlc > 8 && !fd) {
        _cbFD->setChecked(true);
        return;
    }

    _can_msg.setId(id);
    _can_msg.setExtended(_cbExtended->isChecked());
    _can_msg.setRTR(_cbRTR->isChecked());
    _can_msg.setFD(fd);
    _can_msg.setBRS(_cbBRS->isChecked());
    _can_msg.setErrorFrame(false);
    _can_msg.setLength(dlc);
    _can_msg.setInterfaceId(_slavedInterfaceId);

    // Read data bytes from grid
    for (int i = 0; i < 64; i++) {
        int r = i / DataCols;
        int c = i % DataCols;
        QTableWidgetItem *item = _dataTable->item(r, c);
        uint8_t val = 0;
        if (item && i < dlc) {
            val = static_cast<uint8_t>(item->text().toUInt(nullptr, 16));
        }
        _can_msg.setDataAt(i, val);
    }

    _can_msg.setRX(false);
    _can_msg.setShow(true);

    updateSignalTable();
    emit messageUpdated(_can_msg);
}

void RawTxWindow::updateSignalTable()
{
    _signalTable->blockSignals(true);

    if (!_currentDbMsg) {
        _signalTable->setRowCount(0);
        _signalTable->blockSignals(false);
        return;
    }

    CanDbSignalList sigList = _currentDbMsg->getSignals();

    if (_signalTable->rowCount() != sigList.size()) {
        // Full rebuild when the signal structure changes (new message selected)
        _signalTable->setRowCount(0);
        _signalTable->setRowCount(sigList.size());

        for (int i = 0; i < sigList.size(); i++) {
            CanDbSignal *sig = sigList[i];

            auto *nameItem = new QTableWidgetItem(sig->name());
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
            nameItem->setToolTip(sig->startBitDescription());
            _signalTable->setItem(i, 0, nameItem);

            _signalTable->setItem(i, 1, new QTableWidgetItem(QString::number(sig->extractPhysicalFromMessage(_can_msg), 'f', 2)));

            auto *unitItem = new QTableWidgetItem(sig->getUnit());
            unitItem->setFlags(unitItem->flags() & ~Qt::ItemIsEditable);
            _signalTable->setItem(i, 2, unitItem);
        }
    } else {
        // Update values in-place — avoids destroying the active editor item
        for (int i = 0; i < sigList.size(); i++) {
            double val = sigList[i]->extractPhysicalFromMessage(_can_msg);
            if (QTableWidgetItem *item = _signalTable->item(i, 1)) {
                item->setText(QString::number(val, 'f', 2));
            }
        }
    }

    _signalTable->blockSignals(false);
}

void RawTxWindow::onSignalValueChanged(int row, int col)
{
    if (col != 1 || !_currentDbMsg || _settingMessage || _updatingSignals) { return; }

    CanDbSignalList sigList = _currentDbMsg->getSignals();
    if (row < 0 || row >= sigList.size()) { return; }

    QTableWidgetItem *item = _signalTable->item(row, col);
    if (!item) { return; }

    bool ok;
    double value = item->text().toDouble(&ok);
    if (!ok) { return; }

    _updatingSignals = true;

    sigList[row]->injectPhysicalIntoMessage(_can_msg, value);

    _dataTable->blockSignals(true);
    int dlc = _can_msg.getLength();
    for (int i = 0; i < dlc && i < 64; i++) {
        int r = i / DataCols;
        int c = i % DataCols;
        QTableWidgetItem *dataItem = _dataTable->item(r, c);
        if (dataItem) {
            dataItem->setText(QString("%1").arg(_can_msg.getByte(i), 2, 16, QChar('0')).toUpper());
        }
    }
    _dataTable->blockSignals(false);

    updateSignalTable();
    emit messageUpdated(_can_msg);

    _updatingSignals = false;
}

void RawTxWindow::setMessage(const BusMessage &msg, const QString &name, BusInterfaceId interfaceId, CanDbMessage *dbMsg)
{
    _settingMessage = true;
    this->setEnabled(true);
    Q_UNUSED(name);

    _slavedInterfaceId = interfaceId;
    _currentDbMsg = dbMsg;

    // Populate interface combo and select the current one
    _comboInterface->blockSignals(true);
    _comboInterface->clear();
    MeasurementSetup &setup = _backend.getSetup();
    for (auto *network : setup.getNetworks()) {
        for (auto *mi : network->interfaces()) {
            BusInterfaceId ifid = mi->busInterface();
            BusInterface *i = _backend.getInterfaceById(ifid);
            if (i) {
                QString label = network->name() + ": " + i->getName();
                _comboInterface->addItem(label, QVariant(ifid));
            }
        }
    }
    for (int i = 0; i < _comboInterface->count(); ++i) {
        if (static_cast<BusInterfaceId>(_comboInterface->itemData(i).toUInt()) == interfaceId) {
            _comboInterface->setCurrentIndex(i);
            break;
        }
    }
    _comboInterface->blockSignals(false);

    // Determine capabilities from interface
    BusInterface *intf = _backend.getInterfaceById(interfaceId);
    bool canfd = intf && (intf->getCapabilities() & BusInterface::capability_canfd);
    populateDlcCombo(canfd);

    _cbFD->setEnabled(canfd);
    _cbBRS->setEnabled(canfd && msg.isFD());
    _cbRTR->setEnabled(!msg.isFD());

    bool isExtended = msg.isExtended();
    bool isFD = msg.isFD();
    bool isBRS = msg.isBRS();
    int dlc = msg.getLength();

    if (dbMsg) {
        isExtended = (dbMsg->getRaw_id() & 0x80000000) != 0;
        int dlcCode = dbMsg->getDlc();
        if (dlcCode <= 8) {
            dlc = dlcCode;
        } else {
            isFD = true;
            static const int dlcToLen[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
            if (dlcCode < 16) {
                dlc = dlcToLen[dlcCode];
            }
        }
    }

    _editId->setText(QString("%1").arg(msg.getId(), 0, 16).toUpper());
    _cbExtended->setChecked(isExtended);
    _cbRTR->setChecked(msg.isRTR());
    _cbFD->setChecked(isFD);
    _cbBRS->setChecked(isBRS);

    int dlcIdx = _comboDlc->findData(dlc);
    if (dlcIdx >= 0) {
        _comboDlc->setCurrentIndex(dlcIdx);
    }

    // Load data bytes into grid
    _dataTable->blockSignals(true);
    for (int i = 0; i < 64; i++) {
        int r = i / DataCols;
        int c = i % DataCols;
        QTableWidgetItem *item = _dataTable->item(r, c);
        if (item) {
            item->setText(QString("%1").arg(msg.getByte(i), 2, 16, QChar('0')).toUpper());
        }
    }
    _dataTable->blockSignals(false);

    updateDataGrid();

    _settingMessage = false;
    onFieldChanged(); // populates _can_msg from the current UI state
}

bool RawTxWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, xml, root)) { return false; }
    root.setAttribute("type", "RawTxWindow");
    return true;
}

bool RawTxWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el)) { return false; }
    return true;
}
