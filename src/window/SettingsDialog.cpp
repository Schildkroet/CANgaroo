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

#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QStyleFactory>
#include <QApplication>
#include <QActionGroup>

SettingsDialog::SettingsDialog(QSettings &settings, QActionGroup *languageGroup, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    setMinimumWidth(350);

    auto *mainLayout = new QVBoxLayout(this);

    // ── Appearance ────────────────────────────────────────────────────────────
    auto *grpAppearance = new QGroupBox(tr("Appearance"), this);
    auto *formAppearance = new QFormLayout(grpAppearance);

    m_nativeStylingCheck = new QCheckBox(tr("Use native system styling (GNOME/Adwaita)"), grpAppearance);
    m_nativeStylingCheck->setToolTip(tr("Defer to the selected style and the desktop theme instead of "
                                        "applying CANgaroo's own palette and colors."));
    m_nativeStylingCheck->setChecked(settings.value("ui/nativeStyling", true).toBool());
    formAppearance->addRow(m_nativeStylingCheck);

    m_themeCombo = new QComboBox(grpAppearance);
    QStringList styles = QStyleFactory::keys();
    m_themeCombo->addItems(styles);
    const QString currentStyle = settings.value("ui/applicationStyle", QApplication::style()->name()).toString();
    for (int i = 0; i < styles.size(); ++i)
    {
        if (styles[i].compare(currentStyle, Qt::CaseInsensitive) == 0)
        {
            m_themeCombo->setCurrentIndex(i);
            break;
        }
    }
    formAppearance->addRow(tr("Theme:"), m_themeCombo);

    m_languageCombo = new QComboBox(grpAppearance);
    QString savedLocale = settings.value("ui/language", "en_US").toString();
    int langIdx = 0;
    if (languageGroup)
    {
        int i = 0;
        for (QAction *action : languageGroup->actions())
        {
            m_languageCombo->addItem(action->text(), action->data());
            if (action->data().toString() == savedLocale)
                langIdx = i;
            ++i;
        }
    }
    m_languageCombo->setCurrentIndex(langIdx);
    formAppearance->addRow(tr("Language:"), m_languageCombo);

    m_fontSizeSpin = new QSpinBox(grpAppearance);
    m_fontSizeSpin->setRange(6, 24);
    int defaultSize = QApplication::font().pointSize();
    if (defaultSize < 6) { defaultSize = 9; }
    m_fontSizeSpin->setValue(settings.value("ui/fontSize", defaultSize).toInt());
    m_fontSizeSpin->setSuffix(" pt");
    formAppearance->addRow(tr("Font size:"), m_fontSizeSpin);

    mainLayout->addWidget(grpAppearance);

    // ── Behavior ──────────────────────────────────────────────────────────────
    auto *grpBehavior = new QGroupBox(tr("Behavior"), this);
    auto *formBehavior = new QFormLayout(grpBehavior);

    m_restoreWindowCheck = new QCheckBox(tr("Restore window layout on startup"), grpBehavior);
    m_restoreWindowCheck->setChecked(settings.value("ui/restoreWindowGeometry", false).toBool());
    formBehavior->addRow(m_restoreWindowCheck);

    m_clearTraceOnStartCheck = new QCheckBox(tr("Clear trace on measurement start"), grpBehavior);
    m_clearTraceOnStartCheck->setChecked(settings.value("ui/clearTraceOnStart", true).toBool());
    formBehavior->addRow(m_clearTraceOnStartCheck);

    m_skipSaveWorkspacePromptCheck = new QCheckBox(tr("Do not ask to save workspace on close (always discard)"), grpBehavior);
    m_skipSaveWorkspacePromptCheck->setChecked(settings.value("ui/skipSaveWorkspacePrompt", false).toBool());
    formBehavior->addRow(m_skipSaveWorkspacePromptCheck);

    mainLayout->addWidget(grpBehavior);

    // ── Trace Window ──────────────────────────────────────────────────────────
    auto *grpTrace = new QGroupBox(tr("Trace Window"), this);
    auto *formTrace = new QFormLayout(grpTrace);

    m_defaultTraceViewCombo = new QComboBox(grpTrace);
    m_defaultTraceViewCombo->addItem(tr("Aggregated"), 0);
    m_defaultTraceViewCombo->addItem(tr("Rolling Log"), 1);
    m_defaultTraceViewCombo->setCurrentIndex(settings.value("tracewindow/defaultViewMode", 0).toInt());
    formTrace->addRow(tr("Default view:"), m_defaultTraceViewCombo);

    m_defaultTimestampCombo = new QComboBox(grpTrace);
    m_defaultTimestampCombo->addItem(tr("Absolute"),       0);
    m_defaultTimestampCombo->addItem(tr("Absolute (UTC)"), 3);
    m_defaultTimestampCombo->addItem(tr("Relative"),       1);
    m_defaultTimestampCombo->addItem(tr("Delta"),          2);
    {
        const int savedTs = settings.value("tracewindow/defaultTimestampMode", 2).toInt();
        for (int i = 0; i < m_defaultTimestampCombo->count(); ++i)
        {
            if (m_defaultTimestampCombo->itemData(i).toInt() == savedTs)
            {
                m_defaultTimestampCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    formTrace->addRow(tr("Default timestamp:"), m_defaultTimestampCombo);

    m_dataDisplayCombo = new QComboBox(grpTrace);
    m_dataDisplayCombo->addItem(tr("Hex"), 0);
    m_dataDisplayCombo->addItem(tr("ASCII"), 1);
    m_dataDisplayCombo->setCurrentIndex(settings.value("tracewindow/dataAsciiMode", false).toBool() ? 1 : 0);
    formTrace->addRow(tr("Data display:"), m_dataDisplayCombo);

    m_saveFormatCombo = new QComboBox(grpTrace);
    const QStringList saveFormats = {
        "Vector ASC (*.asc)",
        "Vector MDF4 (*.mf4)",
        "Linux candump (*.candump)",
        "PCAP (*.pcap)",
        "PCAPng (*.pcapng)"
    };
    m_saveFormatCombo->addItems(saveFormats);
    const QString savedFormat = settings.value("ui/preferredSaveFormat", saveFormats.first()).toString();
    int fmtIdx = saveFormats.indexOf(savedFormat);
    m_saveFormatCombo->setCurrentIndex(fmtIdx >= 0 ? fmtIdx : 0);
    formTrace->addRow(tr("Preferred save format:"), m_saveFormatCombo);

    m_maxTraceSizeSpin = new QSpinBox(grpTrace);
    m_maxTraceSizeSpin->setRange(1000, 10000000);
    m_maxTraceSizeSpin->setSingleStep(10000);
    m_maxTraceSizeSpin->setGroupSeparatorShown(true);
    m_maxTraceSizeSpin->setSuffix(tr(" messages"));
    m_maxTraceSizeSpin->setToolTip(tr("Maximum number of messages kept in memory. When exceeded, the "
                                      "oldest messages are discarded and are missing from saved traces. "
                                      "Larger values use more memory."));
    m_maxTraceSizeSpin->setValue(settings.value("trace/maxSize", 50000).toInt());
    formTrace->addRow(tr("Max trace size:"), m_maxTraceSizeSpin);

    mainLayout->addWidget(grpTrace);

    // ── Decoders ──────────────────────────────────────────────────────────────
    auto *grpDecoders = new QGroupBox(tr("Decoders"), this);
    auto *formDecoders = new QFormLayout(grpDecoders);

    m_uds29BitCheck = new QCheckBox(tr("Decode UDS on 29-bit (extended) CAN IDs"), grpDecoders);
    m_uds29BitCheck->setChecked(settings.value("decoder/uds29Bit", true).toBool());
    formDecoders->addRow(m_uds29BitCheck);

    mainLayout->addWidget(grpDecoders);

    mainLayout->addSpacing(10);

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

QString SettingsDialog::selectedTheme() const
{
    return m_themeCombo->currentText();
}

bool SettingsDialog::nativeStylingEnabled() const
{
    return m_nativeStylingCheck->isChecked();
}

QString SettingsDialog::selectedLanguage() const
{
    return m_languageCombo->currentData().toString();
}

bool SettingsDialog::restoreWindowEnabled() const
{
    return m_restoreWindowCheck->isChecked();
}

bool SettingsDialog::clearTraceOnStart() const
{
    return m_clearTraceOnStartCheck->isChecked();
}

int SettingsDialog::selectedFontSize() const
{
    return m_fontSizeSpin->value();
}

bool SettingsDialog::uds29BitEnabled() const
{
    return m_uds29BitCheck->isChecked();
}

bool SettingsDialog::skipSaveWorkspacePrompt() const
{
    return m_skipSaveWorkspacePromptCheck->isChecked();
}

QString SettingsDialog::preferredSaveFormat() const
{
    return m_saveFormatCombo->currentText();
}

int SettingsDialog::defaultTraceViewMode() const
{
    return m_defaultTraceViewCombo->currentData().toInt();
}

int SettingsDialog::defaultTimestampMode() const
{
    return m_defaultTimestampCombo->currentData().toInt();
}

bool SettingsDialog::dataAsciiModeEnabled() const
{
    return m_dataDisplayCombo->currentData().toInt() == 1;
}

int SettingsDialog::maxTraceSize() const
{
    return m_maxTraceSizeSpin->value();
}
