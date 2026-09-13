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
#include <QSettings>

class QComboBox;
class QCheckBox;
class QSpinBox;
class QActionGroup;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QSettings &settings, QActionGroup *languageGroup, QWidget *parent = nullptr);

    QString selectedTheme() const;
    bool nativeStylingEnabled() const;
    QString selectedLanguage() const;
    bool restoreWindowEnabled() const;
    bool clearTraceOnStart() const;
    int selectedFontSize() const;
    bool uds29BitEnabled() const;
    bool skipSaveWorkspacePrompt() const;
    QString preferredSaveFormat() const;
    int defaultTraceViewMode() const;
    int defaultTimestampMode() const;
    bool dataAsciiModeEnabled() const;
    int maxTraceSize() const;

private:
    QComboBox *m_themeCombo;
    QComboBox *m_languageCombo;
    QComboBox *m_saveFormatCombo;
    QComboBox *m_defaultTraceViewCombo;
    QComboBox *m_defaultTimestampCombo;
    QComboBox *m_dataDisplayCombo;
    QCheckBox *m_nativeStylingCheck;
    QCheckBox *m_restoreWindowCheck;
    QCheckBox *m_clearTraceOnStartCheck;
    QCheckBox *m_uds29BitCheck;
    QCheckBox *m_skipSaveWorkspacePromptCheck;
    QSpinBox *m_fontSizeSpin;
    QSpinBox *m_maxTraceSizeSpin;
};
