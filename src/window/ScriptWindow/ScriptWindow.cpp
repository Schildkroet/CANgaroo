/*
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
#include "ScriptWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QFileDialog>
#include <QLineEdit>
#include <QTextStream>
#include <QDomDocument>
#include <QFileInfo>
#include <QFont>
#include <thread>

#include "core/PythonEngine.h"
#include "core/Backend.h"
#include "core/BusTrace.h"


ScriptWindow::ScriptWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(&backend)
{
    _engine = new PythonEngine(backend, this);

    // --- Layout ---
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);

    // Toolbar
    auto *toolbar = new QHBoxLayout();
    _btnRun   = new QPushButton(tr("Run"));
    _btnStop  = new QPushButton(tr("Stop"));
    _btnClear = new QPushButton(tr("Clear"));
    _btnLoad  = new QPushButton(tr("Load"));
    _btnSave  = new QPushButton(tr("Save"));

    _chkAutoRun = new QCheckBox(tr("AutoRun"));
    _chkAutoRun->setToolTip(tr("Start script with measurement"));
    _btnStop->setEnabled(false);

    toolbar->addWidget(_btnRun);
    toolbar->addWidget(_btnStop);
    toolbar->addWidget(_chkAutoRun);
    toolbar->addStretch();
    toolbar->addWidget(_btnLoad);
    toolbar->addWidget(_btnSave);
    toolbar->addWidget(_btnClear);

    mainLayout->addLayout(toolbar);

    // File path display
    _fileLabel = new QLineEdit(this);
    _fileLabel->setReadOnly(true);
    _fileLabel->setPlaceholderText(tr("No script loaded"));
    _fileLabel->setFrame(false);
    mainLayout->addWidget(_fileLabel);

    // Splitter: editor (top) + console (bottom)
    _splitter = new QSplitter(Qt::Horizontal, this);

    QFont mono("Monospace");
    mono.setStyleHint(QFont::TypeWriter);

    _editor = new QPlainTextEdit(this);
    _editor->setFont(mono);
    _editor->setPlaceholderText(tr("# Python script\nimport cangaroo\n\nfor msg in cangaroo.get_trace(10):\n    print(msg)"));
    _editor->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(' ') * 4);

    _console = new QPlainTextEdit(this);
    _console->setFont(mono);
    _console->setReadOnly(true);
    _console->setPlaceholderText(tr("Script output..."));

    _input = new QLineEdit(this);
    _input->setFont(mono);
    _input->setPlaceholderText(tr("Script input (press Enter to send)..."));
    _input->setEnabled(false);

    _splitter->addWidget(_editor);
    _splitter->addWidget(_console);
    _splitter->setStretchFactor(0, 3);
    _splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(_splitter);
    mainLayout->addWidget(_input);

    // Connections
    connect(_btnRun,   &QPushButton::clicked, this, &ScriptWindow::onRunClicked);
    connect(_btnStop,  &QPushButton::clicked, this, &ScriptWindow::onStopClicked);
    connect(_btnClear, &QPushButton::clicked, this, &ScriptWindow::onClearClicked);
    connect(_btnLoad,  &QPushButton::clicked, this, &ScriptWindow::onLoadClicked);
    connect(_btnSave,  &QPushButton::clicked, this, &ScriptWindow::onSaveClicked);
    connect(_input, &QLineEdit::returnPressed, this, &ScriptWindow::onInputSubmitted);
    connect(_chkAutoRun, &QCheckBox::toggled, this, [this]() { emit settingsChanged(this); });

    connect(_engine, &PythonEngine::scriptOutput,   this, &ScriptWindow::onScriptOutput, Qt::QueuedConnection);
    connect(_engine, &PythonEngine::scriptError,    this, &ScriptWindow::onScriptError, Qt::QueuedConnection);
    connect(_engine, &PythonEngine::scriptStarted,  this, &ScriptWindow::onScriptStarted, Qt::QueuedConnection);
    connect(_engine, &PythonEngine::scriptFinished, this, &ScriptWindow::onScriptFinished, Qt::QueuedConnection);

    connect(&backend, &Backend::beginMeasurement, this, &ScriptWindow::onMeasurementStarted);
    connect(&backend, &Backend::endMeasurement,   this, &ScriptWindow::onMeasurementStopped);

    // Forward incoming CAN messages to the Python engine's receive queue
    BusTrace *trace = backend.getTrace();
    connect(trace, &BusTrace::messageEnqueued, this, [this, trace](int idx)
    {
        if (_engine->isRunning())
        {
            _engine->enqueueMessage(trace->getMessage(idx));
        }
    }, Qt::DirectConnection);
}

ScriptWindow::~ScriptWindow()
{
    // Join any in-flight async stop (see requestStopAsync()) before calling
    // stopScript() again -- it and _engine must not still be in use by that
    // thread when Qt deletes _engine as our child right after this destructor
    // body returns. If no async stop is in flight this call is synchronous,
    // same as before; if one already finished, it's a cheap no-op.
    if (_stopThread.joinable()) { _stopThread.join(); }
    _engine->stopScript();
}

void ScriptWindow::retranslateUi()
{
    _btnRun->setText(tr("Run"));
    _btnStop->setText(tr("Stop"));
    _btnClear->setText(tr("Clear"));
    _btnLoad->setText(tr("Load"));
    _btnSave->setText(tr("Save"));
    _chkAutoRun->setText(tr("AutoRun"));
    _chkAutoRun->setToolTip(tr("Start script with measurement"));
    _input->setPlaceholderText(tr("Script input (press Enter to send)..."));
}

void ScriptWindow::onRunClicked()
{
    reloadIfModified();
    _console->clear();
    _engine->runScript(_editor->toPlainText());
}

void ScriptWindow::onStopClicked()
{
    requestStopAsync();
}

void ScriptWindow::onClearClicked()
{
    _console->clear();
}

void ScriptWindow::loadScriptFile(const QString &filename)
{
    QFile file(filename);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream in(&file);
        _editor->setPlainText(in.readAll());
        _scriptFilePath = filename;
        _fileLabel->setText(filename);
        _lastLoadTime = QFileInfo(filename).lastModified();
    }
}

void ScriptWindow::reloadIfModified()
{
    if (_scriptFilePath.isEmpty()) { return; }
    QFileInfo fi(_scriptFilePath);
    if (!fi.exists()) { return; }
    if (fi.lastModified() > _lastLoadTime)
    {
        loadScriptFile(_scriptFilePath);
    }
}

void ScriptWindow::onLoadClicked()
{
    QString filename = QFileDialog::getOpenFileName(this, tr("Load Python Script"),
                                                     _scriptFilePath, tr("Python Files (*.py);;All Files (*)"));
    if (filename.isEmpty()) { return; }
    loadScriptFile(filename);
    emit settingsChanged(this);
}

void ScriptWindow::onSaveClicked()
{
    QString filename = QFileDialog::getSaveFileName(this, tr("Save Python Script"),
                                                     _scriptFilePath, tr("Python Files (*.py);;All Files (*)"));
    if (filename.isEmpty()) { return; }

    QFile file(filename);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QTextStream out(&file);
        out << _editor->toPlainText();
        _scriptFilePath = filename;
        _fileLabel->setText(filename);
        emit settingsChanged(this);
    }
}

void ScriptWindow::onInputSubmitted()
{
    if (!_engine->isRunning()) { return; }

    const QString text = _input->text();
    _console->moveCursor(QTextCursor::End);
    _console->insertPlainText(QStringLiteral("> ") + text + QStringLiteral("\n"));
    _console->moveCursor(QTextCursor::End);
    _engine->enqueueInput(text);
    _input->clear();
    _input->setFocus();
}

void ScriptWindow::onScriptOutput(const QString &text)
{
    _console->moveCursor(QTextCursor::End);
    _console->insertPlainText(text);
    _console->moveCursor(QTextCursor::End);
}

void ScriptWindow::onScriptError(const QString &text)
{
    _console->moveCursor(QTextCursor::End);
    QTextCharFormat fmt;
    fmt.setForeground(Qt::red);
    _console->mergeCurrentCharFormat(fmt);
    _console->insertPlainText(text);
    fmt.setForeground(_console->palette().color(QPalette::Text));
    _console->mergeCurrentCharFormat(fmt);
    _console->moveCursor(QTextCursor::End);
}

void ScriptWindow::onScriptStarted()
{
    _btnRun->setEnabled(false);
    _btnStop->setEnabled(true);
    _editor->setReadOnly(true);
    _input->setEnabled(true);
}

void ScriptWindow::onScriptFinished()
{
    _stopInProgress = false;
    _btnRun->setEnabled(true);
    _btnStop->setEnabled(false);
    _editor->setReadOnly(false);
    _input->clear();
    _input->setEnabled(false);
}

void ScriptWindow::onMeasurementStarted()
{
    if (_chkAutoRun->isChecked() && !_engine->isRunning())
    {
        reloadIfModified();
        _console->clear();
        _engine->runScript(_editor->toPlainText());
    }
}

void ScriptWindow::onMeasurementStopped()
{
    if (_engine->isRunning())
    {
        requestStopAsync();
    }
}

// PythonEngine::stopScript() blocks for up to ~5s waiting for the worker
// thread to notice the stop request (e.g. while a script is inside
// time.sleep() or a long-timeout cangaroo.receive() call). Calling it
// directly from a GUI slot freezes the whole application for that long.
// The Stop button itself doesn't need to wait for that: onScriptFinished()
// already reacts to PythonEngine::scriptFinished (queued connection) once
// the worker actually exits, so the wait can happen off the GUI thread.
void ScriptWindow::requestStopAsync()
{
    if (_stopInProgress || !_engine->isRunning()) { return; }
    _stopInProgress = true;
    _btnStop->setEnabled(false);

    // Join a previous stop thread (from an earlier run) before replacing it --
    // by this point it has necessarily already finished, since onScriptFinished()
    // is what cleared _stopInProgress and made this call reachable again.
    if (_stopThread.joinable()) { _stopThread.join(); }

    PythonEngine *engine = _engine;
    _stopThread = std::thread([engine]() { engine->stopScript(); });
}

bool ScriptWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    (void) backend;
    (void) xml;
    root.setAttribute("file", _scriptFilePath);
    root.setAttribute("autorun", _chkAutoRun->isChecked() ? "1" : "0");
    return true;
}

bool ScriptWindow::loadXML(Backend &backend, QDomElement &el)
{
    (void) backend;
    QString filepath = el.attribute("file");
    if (!filepath.isEmpty())
    {
        loadScriptFile(filepath);
    }
    _chkAutoRun->blockSignals(true);
    _chkAutoRun->setChecked(el.attribute("autorun", "0") == "1");
    _chkAutoRun->blockSignals(false);
    return true;
}
