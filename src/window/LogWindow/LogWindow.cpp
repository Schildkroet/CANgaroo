/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
  Copyright (c) 2026 Schildkroet

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

#include "LogWindow.h"
#include "ui_LogWindow.h"

#include <algorithm>

#include <QDomDocument>
#include <QFileDialog>
#include <QHeaderView>
#include <QResizeEvent>
#include <QStyle>
#include <QTextStream>
#include <QFile>
#include "core/Backend.h"
#include "core/LogModel.h"

LogWindow::LogWindow(QWidget *parent, Backend &backend) :
    ConfigurableWidget(parent),
    ui(new Ui::LogWindow),
    _backend(&backend)
{
    ui->setupUi(this);

    connect(&backend.getLogModel(), &QAbstractItemModel::rowsInserted, this, &LogWindow::rowsInserted);

    ui->treeView->setModel(&backend.getLogModel());

    // A stretched last column is clamped to the viewport, so long messages were
    // cut off with no horizontal scrollbar. Size the text column to its content instead.
    ui->treeView->header()->setStretchLastSection(false);
    ui->treeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(&backend.getLogModel(), &QAbstractItemModel::modelReset, this, [this]()
    {
        _textContentWidth = 0;
        updateTextColumnWidth();
    });
    rowsInserted(QModelIndex(), 0, backend.getLogModel().rowCount(QModelIndex()) - 1);

    _scroll_timer.setInterval(1);
    _scroll_timer.setSingleShot(true);
    connect(&_scroll_timer, &QTimer::timeout, this, &LogWindow::_scroll_timer_timeout);
}

LogWindow::~LogWindow()
{
    delete ui;
}

void LogWindow::retranslateUi()
{
    ui->retranslateUi(this);
}

bool LogWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, xml, root)) { return false; }
    root.setAttribute("type", "LogWindow");
    return true;
}

bool LogWindow::loadXML(Backend &backend, QDomElement &el)
{
    return ConfigurableWidget::loadXML(backend, el);
}

void LogWindow::rowsInserted(const QModelIndex &parent, int first, int last)
{
    const QAbstractItemModel &model = _backend->getLogModel();
    const QFontMetrics metrics(ui->treeView->font());
    // Cell padding plus room for the item's focus frame
    const int padding = 2 * ui->treeView->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, ui->treeView) + 12;

    for (int row = first; row <= last; ++row)
    {
        const QString text = model.data(model.index(row, LogModel::column_text, parent), Qt::DisplayRole).toString();
        _textContentWidth = std::max(_textContentWidth, metrics.horizontalAdvance(text) + padding);
    }
    updateTextColumnWidth();

    _scroll_timer.start();
}

void LogWindow::resizeEvent(QResizeEvent *event)
{
    ConfigurableWidget::resizeEvent(event);
    updateTextColumnWidth();
}

void LogWindow::updateTextColumnWidth()
{
    QHeaderView *header = ui->treeView->header();
    const int otherColumns = header->sectionSize(LogModel::column_time) + header->sectionSize(LogModel::column_level);
    // Never narrower than the viewport, so short logs look like before
    const int fill = ui->treeView->viewport()->width() - otherColumns;
    header->resizeSection(LogModel::column_text, std::max(_textContentWidth, fill));
}

void LogWindow::_scroll_timer_timeout()
{
    ui->treeView->scrollToBottom();
}

void LogWindow::on_btnExport_clicked()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export Logs"), "", tr("Log Files (*.log);;Text Files (*.txt);;All Files (*)"));
    if (fileName.isEmpty()) {
        return;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    QTextStream out(&file);
    LogModel &model = _backend->getLogModel();
    int rows = model.rowCount(QModelIndex());
    
    // Header
    out << model.headerData(LogModel::column_time, Qt::Horizontal, Qt::DisplayRole).toString() << "\t"
        << model.headerData(LogModel::column_level, Qt::Horizontal, Qt::DisplayRole).toString() << "\t"
        << model.headerData(LogModel::column_text, Qt::Horizontal, Qt::DisplayRole).toString() << "\n";
    out << "------------------------------------------------------------\n";

    for (int i = 0; i < rows; ++i) {
        QString time = model.data(model.index(i, LogModel::column_time, QModelIndex()), Qt::DisplayRole).toString();
        QString level = model.data(model.index(i, LogModel::column_level, QModelIndex()), Qt::DisplayRole).toString();
        QString text = model.data(model.index(i, LogModel::column_text, QModelIndex()), Qt::DisplayRole).toString();
        out << time << "\t" << level << "\t" << text << "\n";
    }

    file.close();
}

void LogWindow::on_btnClear_clicked()
{
    _backend->getLogModel().clear();
}
