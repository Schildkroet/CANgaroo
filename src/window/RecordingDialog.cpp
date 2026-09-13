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

#include "RecordingDialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

RecordingDialog::RecordingDialog(const RecordingConfig &config, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Record Trace"));
    setMinimumWidth(480);

    auto *mainLayout = new QVBoxLayout(this);

    // ── Output ────────────────────────────────────────────────────────────────
    auto *grpOutput = new QGroupBox(tr("Output"), this);
    auto *formOutput = new QFormLayout(grpOutput);

    auto *folderRow = new QHBoxLayout;
    m_folderEdit = new QLineEdit(config.folder, grpOutput);
    auto *btnBrowse = new QPushButton(tr("Browse..."), grpOutput);
    folderRow->addWidget(m_folderEdit, 1);
    folderRow->addWidget(btnBrowse);
    formOutput->addRow(tr("Folder:"), folderRow);

    m_patternEdit = new QLineEdit(config.fileNamePattern, grpOutput);
    m_patternEdit->setToolTip(tr("Placeholders: {date}, {time} (recording start) and {index} (file number when splitting)"));
    formOutput->addRow(tr("File name:"), m_patternEdit);

    m_formatCombo = new QComboBox(grpOutput);
    m_formatCombo->addItem(tr("Vector ASC (*.asc)"), static_cast<int>(TraceFileFormat::VectorAsc));
    m_formatCombo->addItem(tr("Linux candump (*.candump)"), static_cast<int>(TraceFileFormat::CanDump));
    m_formatCombo->addItem(tr("PCAPng (*.pcapng)"), static_cast<int>(TraceFileFormat::PcapNg));
    m_formatCombo->setCurrentIndex(std::max(m_formatCombo->findData(static_cast<int>(config.format)), 0));
    formOutput->addRow(tr("Format:"), m_formatCombo);

    m_previewLabel = new QLabel(grpOutput);
    m_previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_previewLabel->setWordWrap(true);
    formOutput->addRow(tr("Example:"), m_previewLabel);

    mainLayout->addWidget(grpOutput);

    // ── Split files ───────────────────────────────────────────────────────────
    auto *grpSplit = new QGroupBox(tr("Split Files"), this);
    auto *splitRow = new QHBoxLayout(grpSplit);

    m_splitCheck = new QCheckBox(tr("Start a new file every"), grpSplit);
    m_splitCheck->setChecked(config.splitSizeMb > 0);
    m_splitSizeSpin = new QSpinBox(grpSplit);
    m_splitSizeSpin->setRange(1, 100000);
    m_splitSizeSpin->setSuffix(tr(" MB"));
    m_splitSizeSpin->setValue(config.splitSizeMb > 0 ? config.splitSizeMb : 100);
    splitRow->addWidget(m_splitCheck);
    splitRow->addWidget(m_splitSizeSpin);
    splitRow->addStretch(1);

    mainLayout->addWidget(grpSplit);

    // ── Start ─────────────────────────────────────────────────────────────────
    auto *grpStart = new QGroupBox(tr("Start"), this);
    auto *startLayout = new QVBoxLayout(grpStart);

    m_stayArmedCheck = new QCheckBox(tr("Record every measurement (stay armed after stop)"), grpStart);
    m_stayArmedCheck->setChecked(config.stayArmed);
    startLayout->addWidget(m_stayArmedCheck);

    auto *hint = new QLabel(tr("Recording starts with the next measurement, or immediately if one is running. "
                               "Changed options apply from the next recording."), grpStart);
    hint->setWordWrap(true);
    startLayout->addWidget(hint);

    mainLayout->addWidget(grpStart);
    mainLayout->addSpacing(10);

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    connect(btnBrowse, &QPushButton::clicked, this, &RecordingDialog::browseFolder);
    connect(m_folderEdit, &QLineEdit::textChanged, this, &RecordingDialog::updateState);
    connect(m_patternEdit, &QLineEdit::textChanged, this, &RecordingDialog::updateState);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &RecordingDialog::updateState);
    connect(m_splitCheck, &QCheckBox::toggled, this, &RecordingDialog::updateState);
    updateState();
}

RecordingConfig RecordingDialog::config() const
{
    return RecordingConfig{
        .folder = QDir::cleanPath(m_folderEdit->text().trimmed()),
        .fileNamePattern = m_patternEdit->text().trimmed(),
        .format = static_cast<TraceFileFormat>(m_formatCombo->currentData().toInt()),
        .splitSizeMb = m_splitCheck->isChecked() ? m_splitSizeSpin->value() : 0,
        .stayArmed = m_stayArmedCheck->isChecked(),
    };
}

void RecordingDialog::browseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Recording Folder"), m_folderEdit->text());
    if (!folder.isEmpty())
    {
        m_folderEdit->setText(QDir::toNativeSeparators(folder));
    }
}

void RecordingDialog::updateState()
{
    m_splitSizeSpin->setEnabled(m_splitCheck->isChecked());
    m_previewLabel->setText(TraceRecorder::fileNameFor(config(), QDateTime::currentDateTime(), 1));
    m_okButton->setEnabled(!m_folderEdit->text().trimmed().isEmpty());
}
