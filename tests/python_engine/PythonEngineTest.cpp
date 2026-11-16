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

// PythonEngine lifetime: one embedded CPython interpreter per process.
//
// Every Trace tab owns a ScriptWindow and with it a PythonEngine. Issue #50: a
// workspace with two Trace tabs crashed (0xC0000005) because the second engine
// started a second py::scoped_interpreter. pybind11 refuses that, but it first
// asserts !PyErr_Occurred() while the first engine has released the GIL, so the
// main thread has no thread state and libpython dereferences null.

#include <QtTest>

#include <memory>

#include "core/Backend.h"
#include "core/PythonEngine.h"

namespace
{

// Collects an engine's signals on the test thread. The engine emits from its
// worker thread, so connect queued and poll with QTRY_*.
struct EngineLog
{
    QString out;
    QString err;
    int started{0};
    int finished{0};

    void attach(PythonEngine &engine, QObject *context)
    {
        QObject::connect(&engine, &PythonEngine::scriptOutput, context,
                         [this](const QString &t) { out += t; }, Qt::QueuedConnection);
        QObject::connect(&engine, &PythonEngine::scriptError, context,
                         [this](const QString &t) { err += t; }, Qt::QueuedConnection);
        QObject::connect(&engine, &PythonEngine::scriptStarted, context,
                         [this]() { ++started; }, Qt::QueuedConnection);
        QObject::connect(&engine, &PythonEngine::scriptFinished, context,
                         [this]() { ++finished; }, Qt::QueuedConnection);
    }
};

// Runs until stopped; the engine's trace hook raises KeyboardInterrupt on stop.
constexpr auto kEndlessScript = "import time\nwhile True:\n    time.sleep(0.01)\n";

} // namespace

class PythonEngineTest : public QObject
{
    Q_OBJECT

private:
    Backend _backend;

    // Runs one script to completion and returns its stdout.
    QString runToEnd(PythonEngine &engine, const QString &code)
    {
        EngineLog log;
        QObject context;
        log.attach(engine, &context);
        engine.runScript(code);
        [&]() { QTRY_COMPARE_WITH_TIMEOUT(log.finished, 1, 10000); }();
        if (!log.err.isEmpty()) { qWarning() << "script error:" << log.err; }
        return log.out;
    }

private slots:
    // Issue #50: the second engine used to crash the process in its constructor.
    void twoEnginesBothRunScripts()
    {
        PythonEngine first(_backend);
        PythonEngine second(_backend);

        QCOMPARE(runToEnd(first, "print('first')"), QStringLiteral("first\n"));
        QCOMPARE(runToEnd(second, "print('second')"), QStringLiteral("second\n"));
        QCOMPARE(runToEnd(first, "print('first again')"), QStringLiteral("first again\n"));
    }

    // The embedded module routes every call through one active engine, so a
    // second script must be refused with a message, not run against the wrong tab.
    void secondScriptRefusedWhileFirstRuns()
    {
        PythonEngine first(_backend);
        PythonEngine second(_backend);
        EngineLog firstLog;
        EngineLog secondLog;
        firstLog.attach(first, this);
        secondLog.attach(second, this);

        first.runScript(kEndlessScript);
        QTRY_COMPARE(firstLog.started, 1);

        second.runScript("print('must not run')");
        QVERIFY(!second.isRunning());
        QTRY_VERIFY(secondLog.err.contains("already running"));
        QCOMPARE(secondLog.started, 0);
        QVERIFY(secondLog.out.isEmpty());
        QVERIFY(first.isRunning());

        first.stopScript();
        QVERIFY(!first.isRunning());
        QTRY_COMPARE(firstLog.finished, 1);

        QCOMPARE(runToEnd(second, "print('now it runs')"), QStringLiteral("now it runs\n"));
    }

    // Closing the tab that created the interpreter must not finalize it under
    // the other tabs.
    void interpreterOutlivesTheEngineThatCreatedIt()
    {
        auto first = std::make_unique<PythonEngine>(_backend);
        PythonEngine second(_backend);
        QCOMPARE(runToEnd(*first, "x = 1\nprint(x)"), QStringLiteral("1\n"));

        first.reset();

        QCOMPARE(runToEnd(second, "import sys\nprint(sys.version_info.major)"),
                 QStringLiteral("3\n"));
    }

    // Closing the last Trace tab and opening a new one starts a fresh interpreter.
    void newEngineAfterAllWereDestroyed()
    {
        {
            PythonEngine engine(_backend);
            QCOMPARE(runToEnd(engine, "print('before')"), QStringLiteral("before\n"));
        }
        PythonEngine engine(_backend);
        QCOMPARE(runToEnd(engine, "print('after')"), QStringLiteral("after\n"));
    }
};

QTEST_GUILESS_MAIN(PythonEngineTest)
#include "PythonEngineTest.moc"
