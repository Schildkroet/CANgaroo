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

#include "core/TraceRecorder.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class RecordingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RecordingDialog(const RecordingConfig &config, QWidget *parent = nullptr);

    [[nodiscard]] RecordingConfig config() const;

private:
    QLineEdit *m_folderEdit;
    QLineEdit *m_patternEdit;
    QComboBox *m_formatCombo;
    QLabel *m_previewLabel;
    QCheckBox *m_splitCheck;
    QSpinBox *m_splitSizeSpin;
    QCheckBox *m_stayArmedCheck;
    QPushButton *m_okButton;

    void browseFolder();
    void updateState();
};
