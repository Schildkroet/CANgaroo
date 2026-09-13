/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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
#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QtWidgets>
#include <QMdiArea>
#include <QCloseEvent>
#include <QTimer>
#include <QLabel>
#include <QRegularExpression>
#include <QDockWidget>
#include <QStatusBar>
#include <QDomDocument>
#include <QPalette>
#include <QStyleHints>
#include <QPainter>
#include <QSvgRenderer>
#include <QActionGroup>
#include <QEvent>
#include <QFileInfo>
#include <QToolButton>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QUrl>

#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/ThemeManager.h"
#include "core/TraceRecorder.h"
#include "window/TraceWindow/TraceWindow.h"
#include "window/SetupDialog/SetupDialog.h"
#include "window/LogWindow/LogWindow.h"
#include "window/GraphWindow/GraphWindow.h"
#include "window/CanStatusWindow/CanStatusWindow.h"
#include "window/RawTxWindow/RawTxWindow.h"
#include "window/TxGeneratorWindow/TxGeneratorWindow.h"
#include "window/ScriptWindow/ScriptWindow.h"
#include "window/ReplayWindow/ReplayWindow.h"
#include "window/LinControlWindow/LinControlWindow.h"
#include "window/GpioControlWindow/GpioControlWindow.h"
#include "window/GatewayWindow/GatewayWindow.h"
#include "window/SettingsDialog.h"
#include "window/RecordingDialog.h"
#include "helpers/apphelpers.h"

#include "driver/SLCANDriver/SLCANDriver.h"
#include "driver/GrIPDriver/GrIPDriver.h"
#include "driver/CANBlastDriver/CANBlasterDriver.h"

#if defined(__linux__)
#include <unistd.h>
#include "driver/SocketCanDriver/SocketCanDriver.h"
#else
#include "driver/CandleApiDriver/CandleApiDriver.h"
#endif

#ifdef VECTOR_DRIVER
#include "driver/VectorDriver/VectorDriver.h"
#endif

#ifdef TINYCAN_DRIVER
#include "driver/TinyCanDriver/TinyCanDriver.h"
#endif

#ifdef PEAKCAN_DRIVER
#include "driver/PeakCanDriver/PeakCanDriver.h"
#endif

#ifdef KVASER_DRIVER
#include "driver/KvaserDriver/KvaserDriver.h"
#endif

#if defined(_WIN32)
#ifdef ZSCANFD_DRIVER
#include "driver/ZsCanFdDriver/ZsCanFdDriver.h"
#include <QFile>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#endif
#endif


MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent),
                                          ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    _baseWindowTitle = windowTitle();

    initVersion();
    initActions();
    initDrivers();
    initGeometry();
    initWorkspace();
    initAppearance();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::initVersion()
{
    QCoreApplication::setApplicationVersion(VERSION_STRING);

    auto *versionLabel = new QLabel(this);
    versionLabel->setText(QString("v%1").arg(QCoreApplication::applicationVersion()));
    versionLabel->setStyleSheet("padding-right: 15px; font-weight: bold; font-size: 12px;");
    statusBar()->addPermanentWidget(versionLabel);

    setWindowIcon(QIcon(":/assets/cangaroo.png"));
}

void MainWindow::initActions()
{
    connect(ui->action_Trace_View, &QAction::triggered, this, [this]() { createTraceWindow(); });
    connect(ui->actionLog_View, &QAction::triggered, this, [this]() { addLogWidget(); });
    connect(ui->actionGraph_View, &QAction::triggered, this, [this]() { createGraphWindow(); });
    connect(ui->actionGraph_View_2, &QAction::triggered, this, [this]() { addGraphWidget(); });
    connect(ui->actionSetup, &QAction::triggered, this, &MainWindow::showSetupDialog);
    connect(ui->actionTransmit_View, &QAction::triggered, this, [this]() { addRawTxWidget(); });
    connect(ui->actionGenerator_View, &QAction::triggered, this, [this]() { addTxGeneratorWidget(); });
    connect(ui->actionScript_View, &QAction::triggered, this, [this]() { addScriptWidget(); });
    connect(ui->actionReplay_View, &QAction::triggered, this, [this]() { addReplayWidget(); });
    connect(ui->actionLin_Control_View, &QAction::triggered, this, [this]() { addLinControlWidget(); });
    connect(ui->actionGpio_Control_View, &QAction::triggered, this, [this]() { addGpioControlWidget(); });
    connect(ui->actionSettings, &QAction::triggered, this, &MainWindow::showSettingsDialog);

    auto *actionStandaloneGraph = new QAction(tr("Standalone Graph"), this);
    actionStandaloneGraph->setShortcut(QKeySequence("Ctrl+Shift+B"));
    ui->menuWindow->addAction(actionStandaloneGraph);
    connect(actionStandaloneGraph, &QAction::triggered, this, &MainWindow::createStandaloneGraphWindow);

    connect(ui->actionStart_Measurement, &QAction::triggered, this, &MainWindow::startMeasurement);
    connect(ui->btnStartMeasurement, &QPushButton::released, this, &MainWindow::startMeasurement);
    connect(ui->actionStop_Measurement, &QAction::triggered, this, &MainWindow::stopMeasurement);
    connect(ui->btnStopMeasurement, &QPushButton::released, this, &MainWindow::stopMeasurement);
    connect(ui->btnSetupMeasurement, &QPushButton::released, this, &MainWindow::showSetupDialog);

    connect(ui->actionReload_Interfaces, &QAction::triggered, this, &MainWindow::reloadInterfaces);

#if defined(_WIN32) && defined(ZSCANFD_DRIVER)
    connect(ui->actionDownloadZsCanFdDlls, &QAction::triggered, this, &MainWindow::downloadZsCanFdDlls);
#else
    ui->actionDownloadZsCanFdDlls->setEnabled(false);
#endif

    connect(&backend(), &Backend::beginMeasurement, this, &MainWindow::updateMeasurementActions);
    connect(&backend(), &Backend::endMeasurement, this, &MainWindow::updateMeasurementActions);
    updateMeasurementActions();

    connect(ui->actionSave_Trace_to_file, &QAction::triggered, this, &MainWindow::saveTraceToFile);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);

    auto *actionExportFull = new QAction(tr("Export full trace"), this);
    connect(actionExportFull, &QAction::triggered, this, &MainWindow::exportFullTrace);
    ui->menu_Trace->addAction(actionExportFull);

    auto *actionImportFull = new QAction(tr("Import full trace"), this);
    connect(actionImportFull, &QAction::triggered, this, &MainWindow::importFullTrace);
    ui->menu_Trace->addAction(actionImportFull);

    // Build "Open Recent" submenu and insert it before "Save Workspace..." in menuFile.
    m_recentFilesMenu = new QMenu(tr("Open Recent"), this);
    ui->menuFile->insertMenu(ui->action_WorkspaceSave, m_recentFilesMenu);
    ui->menuFile->insertSeparator(ui->action_WorkspaceSave);
    updateRecentFilesMenu();

    // Open Standalone Graph Button
    // The icon is set in applyActionIcons() so it tracks the palette.
    _btnOpenGraph = new QPushButton(tr("Graph"), this);
    _btnOpenGraph->setToolTip(tr("Open Standalone Graph Window (Ctrl+Shift+B)"));
    _btnOpenGraph->setCursor(Qt::PointingHandCursor);
    ui->horizontalLayoutControls->insertWidget(4, _btnOpenGraph);
    connect(_btnOpenGraph, &QPushButton::clicked, this, &MainWindow::createStandaloneGraphWindow);

    // Gateway Button
    auto *btnGateway = new QPushButton(tr("Gateway"), this);
    btnGateway->setCursor(Qt::PointingHandCursor);
    ui->horizontalLayoutControls->insertWidget(5, btnGateway);
    connect(btnGateway, &QPushButton::clicked, this, &MainWindow::createGatewayWindow);

    auto updateGatewayButton = [btnGateway, this]() {
        int canIfCount = 0;
        for (auto *net : backend().getSetup().getNetworks())
            for (auto *mi : net->interfaces()) {
                BusInterface *intf = backend().getInterfaceById(mi->busInterface());
                if (intf && intf->busType() == BusType::CAN)
                    ++canIfCount;
            }
        btnGateway->setEnabled(canIfCount >= 2);
    };
    connect(&backend(), &Backend::onSetupChanged, this, updateGatewayButton);
    updateGatewayButton();

    initRecording();
}

void MainWindow::initDrivers()
{
    const bool CANblasterEnabled = settings.value("mainWindow/CANblaster", false).toBool();
    const bool tinyCanEnabled = settings.value("mainWindow/TinyCAN", false).toBool();

    ui->actionCANblaster->setChecked(CANblasterEnabled);
    ui->actionTinyCAN->setChecked(tinyCanEnabled);

    // addCanDriver takes ownership via the reference; the raw pointer is intentionally discarded.
#if defined(__linux__)
    Backend::instance().addCanDriver(*(new SocketCanDriver(Backend::instance())));
#else
    Backend::instance().addCanDriver(*(new CandleApiDriver(Backend::instance())));
#endif

    Backend::instance().addCanDriver(*(new SLCANDriver(Backend::instance())));
    Backend::instance().addCanDriver(*(new GrIPDriver(Backend::instance())));

#ifdef PEAKCAN_DRIVER
    Backend::instance().addCanDriver(*(new PeakCanDriver(Backend::instance())));
#endif

#ifdef KVASER_DRIVER
    Backend::instance().addCanDriver(*(new KvaserDriver(Backend::instance())));
#endif

#ifdef VECTOR_DRIVER
    Backend::instance().addCanDriver(*(new VectorDriver(Backend::instance())));
#endif

#if defined(_WIN32)
#ifdef ZSCANFD_DRIVER
    Backend::instance().addCanDriver(*(new ZsCanFdDriver(Backend::instance())));
    QTimer::singleShot(0, this, &MainWindow::checkZsCanFdDlls);
#endif
#endif

    if (CANblasterEnabled)
        Backend::instance().addCanDriver(*(new CANBlasterDriver(Backend::instance())));

#ifdef TINYCAN_DRIVER
    if (tinyCanEnabled)
        Backend::instance().addCanDriver(*(new TinyCanDriver(Backend::instance())));
#endif
}

void MainWindow::initGeometry()
{
    const bool restoreEnabled = settings.value("ui/restoreWindowGeometry", false).toBool();
    ui->actionRestore_Window->setChecked(restoreEnabled);

    if (!restoreEnabled)
        return;

    if (!restoreGeometry(settings.value("mainWindow/geometry").toByteArray()))
    {
        resize(1365, 900);
        if (QScreen *screen = QGuiApplication::primaryScreen())
            move(screen->availableGeometry().center() - rect().center());
        settings.setValue("mainWindow/maximized", false);
    }
    restoreState(settings.value("mainWindow/state").toByteArray());
}

void MainWindow::initWorkspace()
{
    setWorkspaceModified(false);
    newWorkspace();

    // Restore each tab's inner dock layout after newWorkspace() creates them.
    // Must be deferred via singleShot(0) so it fires after the resizeDocks()
    // timer registered inside createTraceWindow() — both use timeout 0,
    // Qt processes them FIFO, so this restore always wins.
    if (settings.value("ui/restoreWindowGeometry", false).toBool())
    {
        for (int i = 0; i < ui->mainTabs->count(); i++)
        {
            QMainWindow *tab = qobject_cast<QMainWindow *>(ui->mainTabs->widget(i));
            if (!tab)
                continue;
            const QByteArray tabState = settings.value(QString("mainWindow/tab_%1_state").arg(i)).toByteArray();
            if (!tabState.isEmpty())
                QTimer::singleShot(0, tab, [tab, tabState]() { tab->restoreState(tabState); });
        }
    }

    // Must be called after drivers/plugins are initialized.
    _setupDlg = new SetupDialog(Backend::instance(), this);
}

void MainWindow::initAppearance()
{
    qApp->installTranslator(&m_translator);
    createLanguageMenu();

    // Load saved application style/theme.
    const QString savedStyle = settings.value("ui/applicationStyle", "").toString();
    if (!savedStyle.isEmpty())
    {
        const QStringList availableStyles = QStyleFactory::keys();
        const bool styleFound = std::any_of(availableStyles.begin(), availableStyles.end(),
            [&](const QString &s) { return s.compare(savedStyle, Qt::CaseInsensitive) == 0; });
        if (styleFound)
            QApplication::setStyle(QStyleFactory::create(savedStyle));
        else
            qWarning() << "Saved style" << savedStyle << "is not available, keeping"
                       << QApplication::style()->name() << "- available styles:" << availableStyles;
    }

    // Style must be set before applyTheme: ThemeManager resets the palette to
    // style()->standardPalette(), which needs the correct style active.
    // applyCurrentTheme() also (re)applies the action icons so they recolor
    // with the active palette.
    applyControlButtonStyles();
    applyCurrentTheme();

    // Follow live light/dark switches of the desktop (e.g. GNOME appearance toggle).
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this](Qt::ColorScheme) { applyCurrentTheme(); });
#endif

    // Load saved font size.
    const int savedFontSize = settings.value("ui/fontSize", 6).toInt();
    if (savedFontSize > 6)
        applyFontSize(savedFontSize);

    // Load saved trace size limit.
    backend().getTrace()->setMaxSize(settings.value("trace/maxSize", 50000).toInt());
}

void MainWindow::applyCurrentTheme()
{
    const bool nativeStyling = settings.value("ui/nativeStyling", true).toBool();

    // In native mode the dark/light decision must follow the active style's
    // palette (e.g. choosing the "adwaita-dark" style), so the app's content
    // colors match what the style actually paints. In the bundled (non-native)
    // theme we follow the desktop's color scheme instead.
    bool dark;
    if (nativeStyling)
    {
        const QPalette pal = QApplication::style()->standardPalette();
        dark = pal.color(QPalette::Window).lightness() < 128;
    }
    else
    {
        dark = isDarkMode();
    }

    ThemeManager::instance().applyTheme(dark ? ThemeManager::Dark : ThemeManager::Light,
                                        nativeStyling);

    applyTabBackgrounds();
    applyActionIcons();
}

void MainWindow::applyTabBackgrounds()
{
    // The tab pages get a subtle background derived from the active palette so
    // they read as a distinct canvas without ever becoming a hard-coded light
    // patch in a dark theme.
    const QColor bg = tabBackgroundColor();
    for (int i = 0; i < ui->mainTabs->count(); ++i)
    {
        QWidget *page = ui->mainTabs->widget(i);
        if (!page)
            continue;
        QPalette pal(page->palette());
        pal.setColor(QPalette::Window, bg);
        page->setAutoFillBackground(true);
        page->setPalette(pal);
    }
}

QColor MainWindow::tabBackgroundColor() const
{
    const QColor base = QApplication::palette().color(QPalette::Window);
    return ThemeManager::instance().isDarkMode() ? base.darker(112) : base.darker(108);
}

QIcon MainWindow::symbolicIcon(const QString &name) const
{
    // Render a bundled Adwaita symbolic SVG recolored to the current text color.
    // This is independent of the SVG's own fill, and guarantees visible icons in
    // both light and dark themes — and inside the AppImage, where no desktop
    // icon theme is available to QIcon::fromTheme.
    return AppHelpers::recoloredSvgIcon(QStringLiteral(":/assets/icons/%1-symbolic.svg").arg(name),
                                        QApplication::palette().color(QPalette::WindowText));
}

void MainWindow::applyActionIcons()
{
    // Prefer the desktop's icon theme (freedesktop names) so a native install
    // matches the rest of the system; fall back to a bundled, palette-recolored
    // symbolic icon so menus/toolbars still have icons inside the AppImage.
    const auto setIcon = [this](QAction *action, const QString &name)
    {
        if (action)
            action->setIcon(QIcon::fromTheme(name, symbolicIcon(name)));
    };

    setIcon(ui->action_WorkspaceNew,      "document-new");
    setIcon(ui->action_WorkspaceOpen,     "document-open");
    setIcon(ui->action_WorkspaceSave,     "document-save");
    setIcon(ui->action_WorkspaceSaveAs,   "document-save-as");
    setIcon(ui->action_Quit,              "application-exit");
    setIcon(ui->actionSettings,           "preferences-system");
    setIcon(ui->actionStart_Measurement,  "media-playback-start");
    setIcon(ui->actionStop_Measurement,   "media-playback-stop");
    setIcon(ui->actionReload_Interfaces,  "view-refresh");
    setIcon(ui->actionSetup,              "preferences-other");
    setIcon(ui->actionAbout,              "help-about");
    setIcon(ui->action_TraceClear,        "edit-clear");
    setIcon(ui->actionSave_Trace_to_file, "document-save");

    if (_btnOpenGraph)
        _btnOpenGraph->setIcon(AppHelpers::themedIcon("office-chart-line", ":/assets/graph.svg"));
}

void MainWindow::applyControlButtonStyles()
{
    // Start/Stop accent colors are applied per-widget (not via the global stylesheet)
    // so they survive native styling, where the global stylesheet is empty.
    ui->btnStartMeasurement->setStyleSheet(
        "QPushButton { background-color: #2e7d32; color: white; border: none;"
        " border-radius: 12px; padding: 5px 15px; font-weight: bold; }"
        "QPushButton:hover { background-color: #388e3c; }"
        "QPushButton:pressed { background-color: #1b5e20; }"
        "QPushButton:disabled { background-color: #88b98a; }");

    ui->btnStopMeasurement->setStyleSheet(
        "QPushButton { background-color: #c62828; color: white; border: none;"
        " border-radius: 12px; padding: 5px 15px; font-weight: bold; }"
        "QPushButton:hover { background-color: #d32f2f; }"
        "QPushButton:pressed { background-color: #8e1c1c; }"
        "QPushButton:disabled { background-color: #d99a9a; }");
}

void MainWindow::addToRecentFiles(const QString &filename)
{
    QStringList recent = settings.value("recentFiles/list").toStringList();
    recent.removeAll(filename);
    recent.prepend(filename);
    while (recent.size() > MaxRecentFiles)
        recent.removeLast();
    settings.setValue("recentFiles/list", recent);
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    m_recentFilesMenu->clear();

    const QStringList recent = settings.value("recentFiles/list").toStringList();
    for (const QString &path : recent)
    {
        QAction *action = m_recentFilesMenu->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path]()
        {
            if (askSaveBecauseWorkspaceModified() != QMessageBox::Cancel)
                loadWorkspaceFromFile(path);
        });
    }

    m_recentFilesMenu->setEnabled(!recent.isEmpty());

    if (!recent.isEmpty())
    {
        m_recentFilesMenu->addSeparator();
        QAction *clearAction = m_recentFilesMenu->addAction(tr("Clear Recent Files"));
        connect(clearAction, &QAction::triggered, this, [this]()
        {
            settings.remove("recentFiles/list");
            updateRecentFilesMenu();
        });
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    const auto cmd = askSaveBecauseWorkspaceModified();
    if (cmd == QMessageBox::Cancel)
    {
        event->ignore();
        return;
    }

    backend().stopMeasurement();
    if (cmd == QMessageBox::Save && !_workspaceFileName.isEmpty())
        saveWorkspaceToFile(_workspaceFileName);

    event->accept();

    settings.setValue("mainWindow/geometry", saveGeometry());
    settings.setValue("mainWindow/state", saveState());
    settings.setValue("mainWindow/maximized", isMaximized());
    settings.setValue("ui/restoreWindowGeometry", ui->actionRestore_Window->isChecked());
    settings.setValue("mainWindow/CANblaster", ui->actionCANblaster->isChecked());
    settings.setValue("mainWindow/TinyCAN", ui->actionTinyCAN->isChecked());

    for (int i = 0; i < ui->mainTabs->count(); i++)
    {
        QMainWindow *tab = qobject_cast<QMainWindow *>(ui->mainTabs->widget(i));
        if (tab)
            settings.setValue(QString("mainWindow/tab_%1_state").arg(i), tab->saveState());
    }

    QMainWindow::closeEvent(event);
}

bool MainWindow::isMaximizedWindow()
{
    return settings.value("mainWindow/maximized").toBool();
}

bool MainWindow::isDarkMode()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Dark;
#else
    const QPalette defaultPalette;
    const auto text = defaultPalette.color(QPalette::WindowText);
    const auto window = defaultPalette.color(QPalette::Window);
    return text.lightness() > window.lightness();
#endif
}

void MainWindow::updateMeasurementActions()
{
    const bool running = backend().isMeasurementRunning();
    ui->actionStart_Measurement->setEnabled(!running);
    ui->actionSetup->setEnabled(!running);
    ui->actionStop_Measurement->setEnabled(running);

    ui->btnStartMeasurement->setEnabled(!running);
    ui->btnSetupMeasurement->setEnabled(!running);
    ui->btnStopMeasurement->setEnabled(running);
}

static RecordingConfig loadRecordingConfig(QSettings &settings)
{
    const RecordingConfig defaults;
    const QString defaultFolder = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                      .filePath(QStringLiteral("CANgaroo"));

    auto format = traceFormatFromName(settings.value("recording/format", traceFormatName(defaults.format)).toString())
                      .value_or(defaults.format);
    if (!TraceRecorder::isFormatSupported(format))
        format = defaults.format;

    return RecordingConfig{
        .folder = settings.value("recording/folder", defaultFolder).toString(),
        .fileNamePattern = settings.value("recording/fileNamePattern", defaults.fileNamePattern).toString(),
        .format = format,
        .splitSizeMb = settings.value("recording/splitSizeMb", defaults.splitSizeMb).toInt(),
        .stayArmed = settings.value("recording/stayArmed", defaults.stayArmed).toBool(),
    };
}

static void saveRecordingConfig(QSettings &settings, const RecordingConfig &config)
{
    settings.setValue("recording/folder", config.folder);
    settings.setValue("recording/fileNamePattern", config.fileNamePattern);
    settings.setValue("recording/format", traceFormatName(config.format));
    settings.setValue("recording/splitSizeMb", config.splitSizeMb);
    settings.setValue("recording/stayArmed", config.stayArmed);
}

void MainWindow::initRecording()
{
    TraceRecorder *recorder = backend().getTraceRecorder();
    recorder->setConfig(loadRecordingConfig(settings));

    _actionRecord = new QAction(tr("&Record Trace to File"), this);
    _actionRecord->setCheckable(true);
    _actionRecord->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    _actionRecord->setIconText(tr("Record"));
    _actionRecord->setToolTip(tr("Continuously record all traffic to a file (Ctrl+R)"));
    _actionRecord->setIcon(QIcon::fromTheme(QStringLiteral("media-record")));

    auto *actionRecordOptions = new QAction(tr("Recording &Options..."), this);
    auto *actionOpenFolder = new QAction(tr("Open Recording &Folder"), this);

    ui->menuMeasurement->insertAction(ui->actionSetup, _actionRecord);
    ui->menuMeasurement->insertAction(ui->actionSetup, actionRecordOptions);
    ui->menuMeasurement->insertSeparator(ui->actionSetup);

    auto *recordMenu = new QMenu(this);
    recordMenu->addAction(actionRecordOptions);
    recordMenu->addAction(actionOpenFolder);

    _btnRecord = new QToolButton(this);
    _btnRecord->setDefaultAction(_actionRecord);
    _btnRecord->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _btnRecord->setPopupMode(QToolButton::MenuButtonPopup);
    _btnRecord->setMenu(recordMenu);
    _btnRecord->setCursor(Qt::PointingHandCursor);
    _btnRecord->setStyleSheet(QStringLiteral(
        "QToolButton:checked { background-color: #c62828; color: white; font-weight: bold;"
        " border: none; border-radius: 4px; padding: 3px 6px; }"));

    _recordStatusLabel = new QLabel(this);
    _recordStatusLabel->setVisible(false);

    // Controls layout: Start, Stop, spacer, Setup, Graph, Gateway, [here], stretch
    ui->horizontalLayoutControls->insertSpacing(6, 30);
    ui->horizontalLayoutControls->insertWidget(7, _btnRecord);
    ui->horizontalLayoutControls->insertWidget(8, _recordStatusLabel);

    _recordStatusTimer = new QTimer(this);
    _recordStatusTimer->setInterval(1000);

    connect(_actionRecord, &QAction::toggled, recorder, &TraceRecorder::setArmed);
    connect(recorder, &TraceRecorder::armedChanged, this, [this](bool armed)
    {
        _actionRecord->setChecked(armed);
        updateRecordingStatus();
    });
    connect(recorder, &TraceRecorder::recordingStarted, this, [this]()
    {
        _recordStatusTimer->start();
        updateRecordingStatus();
    });
    connect(recorder, &TraceRecorder::recordingStopped, this, [this]()
    {
        _recordStatusTimer->stop();
        updateRecordingStatus();
    });
    connect(recorder, &TraceRecorder::fileRotated, this, &MainWindow::updateRecordingStatus);
    connect(_recordStatusTimer, &QTimer::timeout, this, &MainWindow::updateRecordingStatus);

    connect(actionRecordOptions, &QAction::triggered, this, &MainWindow::showRecordingDialog);
    connect(actionOpenFolder, &QAction::triggered, this, [recorder]()
    {
        const QString folder = recorder->config().folder;
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });

    updateRecordingStatus();
}

void MainWindow::showRecordingDialog()
{
    TraceRecorder *recorder = backend().getTraceRecorder();
    RecordingDialog dlg(recorder->config(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const RecordingConfig config = dlg.config();
    saveRecordingConfig(settings, config);
    recorder->setConfig(config);
}

void MainWindow::updateRecordingStatus()
{
    const TraceRecorder *recorder = backend().getTraceRecorder();

    if (recorder->isRecording())
    {
        const qint64 secs = std::max<qint64>(0, recorder->recordingStartTime().secsTo(QDateTime::currentDateTime()));
        const QString elapsed = QStringLiteral("%1:%2:%3")
                                    .arg(secs / 3600, 2, 10, QLatin1Char('0'))
                                    .arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
                                    .arg(secs % 60, 2, 10, QLatin1Char('0'));

        _recordStatusLabel->setText(tr("● REC  %1  ·  %2  ·  %3")
                                        .arg(QFileInfo(recorder->currentFilePath()).fileName(),
                                             locale().formattedDataSize(recorder->totalBytesWritten()),
                                             elapsed));
        _recordStatusLabel->setToolTip(tr("%1\n%2 frames written")
                                           .arg(QDir::toNativeSeparators(recorder->currentFilePath()))
                                           .arg(recorder->framesWritten()));
        _recordStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: #c62828; font-weight: bold; }"));
        _recordStatusLabel->setVisible(true);
    }
    else if (recorder->isArmed())
    {
        _recordStatusLabel->setText(tr("Recording armed, starts with measurement"));
        _recordStatusLabel->setToolTip(QDir::toNativeSeparators(recorder->config().folder));
        _recordStatusLabel->setStyleSheet(QString());
        _recordStatusLabel->setVisible(true);
    }
    else
    {
        _recordStatusLabel->setVisible(false);
    }
}

Backend &MainWindow::backend()
{
    return Backend::instance();
}

QMainWindow *MainWindow::createTab(const QString &title)
{
    auto *mm = new QMainWindow(this);
    QPalette pal(mm->palette());
    pal.setColor(QPalette::Window, tabBackgroundColor());
    mm->setAutoFillBackground(true);
    mm->setPalette(pal);
    ui->mainTabs->addTab(mm, title);
    return mm;
}

QMainWindow *MainWindow::currentTab()
{
    return qobject_cast<QMainWindow *>(ui->mainTabs->currentWidget());
}

void MainWindow::stopAndClearMeasurement()
{
    backend().stopMeasurement();
    // Drain pending cross-thread signals so no in-flight message arrives after the trace is cleared.
    QCoreApplication::processEvents();
    backend().clearTrace();
    backend().clearLog();
}

void MainWindow::clearWorkspace()
{
    while (ui->mainTabs->count() > 0)
    {
        QWidget *w = ui->mainTabs->widget(0);
        ui->mainTabs->removeTab(0);
        delete w;
    }

    // Close standalone windows to prevent dangling pointers to signals.
    while (!_standaloneGraphWindows.isEmpty())
    {
        GraphWindow *gw = _standaloneGraphWindows.takeFirst();
        if (gw)
            gw->close();
    }

    delete _gatewayWindow;
    _gatewayWindow = nullptr;

    _workspaceFileName.clear();
    setWorkspaceModified(false);
}

bool MainWindow::loadWorkspaceTab(QDomElement el)
{
    QMainWindow *mw = nullptr;
    const QString type = el.attribute("type");
    if (type == "TraceWindow")
        mw = createTraceWindow(el.attribute("title"));
    else if (type == "GraphWindow")
        mw = createGraphWindow(el.attribute("title"));
    else
        return false;

    if (mw)
    {
        ConfigurableWidget *mdi = dynamic_cast<ConfigurableWidget *>(mw->centralWidget());
        if (mdi)
            mdi->loadXML(backend(), el);

        // Load TxGeneratorWindow dock content (cyclic frames) if present.
        TxGeneratorWindow *gen = mw->findChild<TxGeneratorWindow *>();
        QDomElement genEl = el.firstChildElement("txgeneratorwindow");
        if (gen && !genEl.isNull())
            gen->loadXML(backend(), genEl);

        // Load ScriptWindow dock content (script code + autorun) if present.
        ScriptWindow *script = mw->findChild<ScriptWindow *>();
        QDomElement scriptEl = el.firstChildElement("scriptwindow");
        if (script && !scriptEl.isNull())
            script->loadXML(backend(), scriptEl);

        // Load GraphWindow dock content (active signals, view type, duration) if present.
        GraphWindow *graph = mw->findChild<GraphWindow *>();
        QDomElement graphEl = el.firstChildElement("graphwindow");
        if (graph && !graphEl.isNull())
            graph->loadXML(backend(), graphEl);

        // Recreate LinControlWindow dock if it was open, and restore diag requests.
        QDomElement linEl = el.firstChildElement("lincontrolwindow");
        if (!linEl.isNull())
        {
            QDockWidget *linDock = addLinControlWidget(mw);
            LinControlWindow *lin = linDock ? qobject_cast<LinControlWindow *>(linDock->widget()) : nullptr;
            if (lin)
                lin->loadXML(backend(), linEl);
        }

        // Recreate GpioControlWindow dock if it was open.
        QDomElement gpioEl = el.firstChildElement("gpiocontrolwindow");
        if (!gpioEl.isNull())
            addGpioControlWidget(mw);

        // Restore dock layout state (splits, tabification, sizes).
        // Deferred so it runs after the default layout timer from createTraceWindow().
        const QString dockState = el.attribute("dockstate");
        if (!dockState.isEmpty())
        {
            QByteArray state = QByteArray::fromBase64(dockState.toLatin1());
            QTimer::singleShot(0, mw, [mw, state]() { mw->restoreState(state); });
        }
    }

    return true;
}

bool MainWindow::loadWorkspaceSetup(QDomElement el)
{
    MeasurementSetup setup(&backend());
    if (setup.loadXML(backend(), el))
    {
        backend().setSetup(setup);
        return true;
    }
    return false;
}

void MainWindow::loadWorkspaceFromFile(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        log_error(QString(tr("Cannot open workspace settings file: %1")).arg(filename));
        return;
    }

    QDomDocument doc;
    if (!doc.setContent(&file))
    {
        file.close();
        log_error(QString(tr("Cannot load settings from file: %1")).arg(filename));
        return;
    }
    file.close();

    stopAndClearMeasurement();
    clearWorkspace();

    QDomElement root = doc.documentElement();
    if (root.tagName() != "cangaroo-workspace")
    {
        log_error(QString("Invalid workspace file format: %1").arg(filename));
        return;
    }

    int workspaceVersion = root.attribute("workspace-version", QString::number(WORKSPACE_VERSION)).toInt();

    QDomElement tabsRoot = root.firstChildElement("tabs");
    QDomNodeList tabs = tabsRoot.elementsByTagName("tab");
    for (int i = 0; i < tabs.length(); i++)
    {
        if (!loadWorkspaceTab(tabs.item(i).toElement()))
            log_warning(QString(tr("Could not read window %1 from file: %2")).arg(QString::number(i), filename));
    }

    QDomElement setupRoot = root.firstChildElement("setup");
    if (loadWorkspaceSetup(setupRoot))
    {
        _workspaceFileName = filename;
        addToRecentFiles(filename);
    }
    else
    {
        log_error(QString(tr("Unable to read measurement setup from workspace config file: %1")).arg(filename));
    }

    QDomElement gwEl = root.firstChildElement("gatewaywindow");
    if (!gwEl.isNull())
    {
        if (!_gatewayWindow)
        {
            _gatewayWindow = new GatewayWindow(nullptr, backend());
            _gatewayWindow->setWindowTitle(tr("CAN Gateway"));
        }
        _gatewayWindow->loadXML(backend(), gwEl);
    }

    if (ui->mainTabs->count() > 0)
        ui->mainTabs->setCurrentIndex(0);

    setWorkspaceModified(false);
}

bool MainWindow::saveWorkspaceToFile(const QString &filename)
{
    QDomDocument doc;
    QDomElement root = doc.createElement("cangaroo-workspace");
    root.setAttribute("cangaroo-version", VERSION_STRING);
    root.setAttribute("workspace-version", WORKSPACE_VERSION);
    doc.appendChild(root);

    QDomElement tabsRoot = doc.createElement("tabs");
    root.appendChild(tabsRoot);

    for (int i = 0; i < ui->mainTabs->count(); i++)
    {
        QMainWindow *w = qobject_cast<QMainWindow *>(ui->mainTabs->widget(i));
        if (!w)
            continue;

        QDomElement tabEl = doc.createElement("tab");
        tabEl.setAttribute("title", ui->mainTabs->tabText(i));

        ConfigurableWidget *mdi = dynamic_cast<ConfigurableWidget *>(w->centralWidget());
        if (!mdi || !mdi->saveXML(backend(), doc, tabEl))
        {
            log_error(QString(tr("Cannot save window settings to file: %1")).arg(filename));
            return false;
        }

        // Save dock layout state so splits/tabification/sizes are preserved.
        tabEl.setAttribute("dockstate", QString::fromLatin1(w->saveState().toBase64()));

        // Save TxGeneratorWindow dock content (cyclic frames) as a sibling element.
        TxGeneratorWindow *gen = w->findChild<TxGeneratorWindow *>();
        if (gen)
        {
            QDomElement genEl = doc.createElement("txgeneratorwindow");
            gen->saveXML(backend(), doc, genEl);
            tabEl.appendChild(genEl);
        }

        // Save ScriptWindow dock content (script code + autorun).
        ScriptWindow *script = w->findChild<ScriptWindow *>();
        if (script)
        {
            QDomElement scriptEl = doc.createElement("scriptwindow");
            script->saveXML(backend(), doc, scriptEl);
            tabEl.appendChild(scriptEl);
        }

        // Save GraphWindow dock content (active signals, view type, duration).
        GraphWindow *graph = w->findChild<GraphWindow *>();
        if (graph)
        {
            QDomElement graphEl = doc.createElement("graphwindow");
            graph->saveXML(backend(), doc, graphEl);
            tabEl.appendChild(graphEl);
        }

        // Save LinControlWindow dock content (diag requests) if present.
        LinControlWindow *lin = w->findChild<LinControlWindow *>();
        if (lin)
        {
            QDomElement linEl = doc.createElement("lincontrolwindow");
            lin->saveXML(backend(), doc, linEl);
            tabEl.appendChild(linEl);
        }

        // Save GpioControlWindow dock if present (marks it should be reopened).
        GpioControlWindow *gpio = w->findChild<GpioControlWindow *>();
        if (gpio)
        {
            QDomElement gpioEl = doc.createElement("gpiocontrolwindow");
            gpio->saveXML(backend(), doc, gpioEl);
            tabEl.appendChild(gpioEl);
        }

        tabsRoot.appendChild(tabEl);
    }

    QDomElement setupRoot = doc.createElement("setup");
    if (!backend().getSetup().saveXML(backend(), doc, setupRoot))
    {
        log_error(QString(tr("Cannot save measurement setup to file: %1")).arg(filename));
        return false;
    }
    root.appendChild(setupRoot);

    if (_gatewayWindow)
    {
        QDomElement gwEl = doc.createElement("gatewaywindow");
        _gatewayWindow->saveXML(backend(), doc, gwEl);
        root.appendChild(gwEl);
    }

    QFile outFile(filename);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        log_error(QString(tr("Cannot open workspace file for writing: %1")).arg(filename));
        return false;
    }

    QTextStream stream(&outFile);
    stream << doc.toString();
    outFile.close();

    _workspaceFileName = filename;
    setWorkspaceModified(false);
    addToRecentFiles(filename);
    log_info(QString(tr("Saved workspace settings to file: %1")).arg(filename));
    return true;
}

void MainWindow::newWorkspace()
{
    if (askSaveBecauseWorkspaceModified() != QMessageBox::Cancel)
    {
        stopAndClearMeasurement();
        clearWorkspace();
        createTraceWindow();
        backend().setDefaultSetup();
        _workspaceFileName.clear();
        setWorkspaceModified(false);
    }
}

void MainWindow::loadWorkspace()
{
    if (askSaveBecauseWorkspaceModified() != QMessageBox::Cancel)
    {
        const QString filename = QFileDialog::getOpenFileName(
            this, tr("Open workspace configuration"), "",
            tr("Workspace config files (*.cangaroo)"));
        if (!filename.isNull())
            loadWorkspaceFromFile(filename);
    }
}

bool MainWindow::saveWorkspace()
{
    if (_workspaceFileName.isEmpty())
        return saveWorkspaceAs();
    return saveWorkspaceToFile(_workspaceFileName);
}

bool MainWindow::saveWorkspaceAs()
{
    QString filename = QFileDialog::getSaveFileName(
        this, tr("Save workspace configuration"), "",
        tr("Workspace config files (*.cangaroo)"));
    if (filename.isNull())
        return false;

    if (!filename.endsWith(".cangaroo", Qt::CaseInsensitive))
        filename += ".cangaroo";

    return saveWorkspaceToFile(filename);
}

void MainWindow::setWorkspaceModified(bool modified)
{
    _workspaceModified = modified;

    QString title = _baseWindowTitle;
    if (!_workspaceFileName.isEmpty())
        title += " - " + QFileInfo(_workspaceFileName).fileName();
    if (_workspaceModified)
        title += '*';

    setWindowTitle(title);
}

int MainWindow::askSaveBecauseWorkspaceModified()
{
    if (!_workspaceModified)
        return QMessageBox::Discard;

    if (settings.value("ui/skipSaveWorkspacePrompt", false).toBool())
        return QMessageBox::Discard;

    QMessageBox msgBox;
    msgBox.setText(tr("The current workspace has been modified."));
    msgBox.setInformativeText(tr("Do you want to save your changes?"));
    msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Save);

    msgBox.button(QMessageBox::Save)->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    msgBox.button(QMessageBox::Discard)->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
    msgBox.button(QMessageBox::Cancel)->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));

    auto *doNotAskCheck = new QCheckBox(tr("Do not ask again (always discard)"), &msgBox);
    msgBox.setCheckBox(doNotAskCheck);

    msgBox.setWindowFlag(Qt::FramelessWindowHint);
    msgBox.setStyleSheet(QStringLiteral("QMessageBox { border: 3px solid palette(highlight); padding: 10px; }"));

    msgBox.adjustSize();
    const QPoint center = mapToGlobal(rect().center());
    msgBox.move(center.x() - msgBox.width() / 2, center.y() - msgBox.height() / 2);

    const int result = msgBox.exec();

    if (doNotAskCheck->isChecked())
        settings.setValue("ui/skipSaveWorkspacePrompt", true);

    if (result == QMessageBox::Save && !saveWorkspace())
        return QMessageBox::Cancel;  // save failed — do not close

    return result;
}

QMainWindow *MainWindow::createTraceWindow(const QString &title)
{
    QMainWindow *mm = createTab(title.isNull() ? tr("Trace") : title);
    auto *trace = new TraceWindow(mm, backend());
    mm->setCentralWidget(trace);

    QDockWidget *dockLogWidget      = addLogWidget(mm);
    QDockWidget *dockStatusWidget   = addStatusWidget(mm);
    QDockWidget *dockGeneratorWidget = addTxGeneratorWidget(mm);
    QDockWidget *dockGraphWidget    = addGraphWidget(mm);
    QDockWidget *dockScriptWidget   = addScriptWidget(mm);
    QDockWidget *dockReplayWidget   = addReplayWidget(mm);

    auto *gen = qobject_cast<TxGeneratorWindow *>(dockGeneratorWidget->widget());
    if (gen)
        connect(gen, &TxGeneratorWindow::loopbackFrame, trace, &TraceWindow::addMessage);

    // splitDockWidget must come before tabifyDockWidget: tabify only works on docks
    // that are already placed in the same area of the same QMainWindow.
    mm->splitDockWidget(dockGeneratorWidget, dockLogWidget, Qt::Horizontal);
    mm->splitDockWidget(dockGraphWidget, dockLogWidget, Qt::Horizontal);
    mm->splitDockWidget(dockScriptWidget, dockLogWidget, Qt::Horizontal);
    mm->splitDockWidget(dockReplayWidget, dockLogWidget, Qt::Horizontal);
    mm->tabifyDockWidget(dockGeneratorWidget, dockGraphWidget);
    mm->tabifyDockWidget(dockGraphWidget, dockScriptWidget);
    mm->tabifyDockWidget(dockScriptWidget, dockReplayWidget);
    mm->splitDockWidget(dockStatusWidget, dockLogWidget, Qt::Horizontal);
    mm->tabifyDockWidget(dockStatusWidget, dockLogWidget);

    // Deferred resize so it runs after the event loop settles.
    QTimer::singleShot(0, mm, [mm, dockLogWidget, dockGeneratorWidget, dockStatusWidget, dockScriptWidget, dockReplayWidget]()
    {
        dockStatusWidget->show();
        dockStatusWidget->raise();
        dockGeneratorWidget->show();
        dockGeneratorWidget->raise();

        mm->resizeDocks({dockLogWidget, dockGeneratorWidget, dockStatusWidget, dockScriptWidget, dockReplayWidget},
                        {600, 600, 600, 600, 600}, Qt::Vertical);
        mm->resizeDocks({dockLogWidget, dockGeneratorWidget, dockStatusWidget, dockScriptWidget, dockReplayWidget},
                        {1200, 1200, 1200, 1200, 1200}, Qt::Horizontal);
    });

    ui->mainTabs->setCurrentWidget(mm);
    return mm;
}

QMainWindow *MainWindow::createGraphWindow(const QString &title)
{
    QMainWindow *mm = createTab(title.isNull() ? tr("Graph") : title);
    mm->setCentralWidget(new GraphWindow(mm, backend()));
    addLogWidget(mm);
    return mm;
}

void MainWindow::createGatewayWindow()
{
    if (!_gatewayWindow) {
        _gatewayWindow = new GatewayWindow(nullptr, backend());
        _gatewayWindow->setWindowTitle(tr("CAN Gateway"));
    }
    _gatewayWindow->show();
    _gatewayWindow->raise();
    _gatewayWindow->activateWindow();
}

void MainWindow::createStandaloneGraphWindow()
{
    auto *gw = new GraphWindow(nullptr, backend());
    gw->setWindowTitle(tr("Standalone Graph"));
    gw->setAttribute(Qt::WA_DeleteOnClose);

    _standaloneGraphWindows.append(gw);
    connect(gw, &QObject::destroyed, this, [this, gw]()
            { _standaloneGraphWindows.removeAll(gw); });

    gw->show();
}

QDockWidget *MainWindow::makeDock(const QString &title, const QString &objectName,
                                   QWidget *content, QMainWindow *parent)
{
    const bool tabify = (parent == nullptr);
    if (!parent)
        parent = currentTab();
    if (!parent)
        return nullptr;

    // Snapshot existing bottom-area docks before adding the new one.
    QDockWidget *tabTarget = nullptr;
    if (tabify)
    {
        const auto existing = parent->findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly);
        for (QDockWidget *d : existing)
        {
            if (!d->isFloating() && parent->dockWidgetArea(d) == Qt::BottomDockWidgetArea)
                tabTarget = d;
        }
    }

    auto *dock = new QDockWidget(title, parent);
    dock->setObjectName(objectName);
    dock->setWidget(content);
    parent->addDockWidget(Qt::BottomDockWidgetArea, dock);

    if (tabTarget)
    {
        parent->tabifyDockWidget(tabTarget, dock);
        dock->show();
        dock->raise();
    }

    setupDockFloatReparent(dock, parent);
    return dock;
}

QDockWidget *MainWindow::addGraphWidget(QMainWindow *parent)
{
    return makeDock(tr("Graph"), QStringLiteral("dock_graph"),
                    new GraphWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addRawTxWidget(QMainWindow *parent)
{
    return makeDock(tr("Message View"), QStringLiteral("dock_rawtx"),
                    new RawTxWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addLogWidget(QMainWindow *parent)
{
    return makeDock(tr("Log"), QStringLiteral("dock_log"),
                    new LogWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addStatusWidget(QMainWindow *parent)
{
    return makeDock(tr("BUS Status"), QStringLiteral("dock_status"),
                    new CanStatusWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addTxGeneratorWidget(QMainWindow *parent)
{
    return makeDock(tr("Generator View"), QStringLiteral("dock_generator"),
                    new TxGeneratorWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addScriptWidget(QMainWindow *parent)
{
    // Only one PythonEngine/interpreter can exist per process (CPython embedding
    // does not support concurrent sub-interpreters here), so reuse an existing
    // Script dock in this tab instead of constructing a second ScriptWindow —
    // doing so would crash inside libpython when the second scoped_interpreter
    // attempt fails without holding the GIL.
    QMainWindow *targetTab = parent ? parent : currentTab();
    if (targetTab)
    {
        if (auto *existing = targetTab->findChild<ScriptWindow *>())
        {
            if (auto *dock = qobject_cast<QDockWidget *>(existing->parentWidget()))
            {
                dock->show();
                dock->raise();
                return dock;
            }
        }
    }

    auto *scriptWindow = new ScriptWindow(nullptr, backend());
    auto *dock = makeDock(tr("Python"), QStringLiteral("dock_script"), scriptWindow, parent);
    if (dock)
        connect(scriptWindow, &ConfigurableWidget::settingsChanged,
                this, [this]() { setWorkspaceModified(true); });
    return dock;
}

QDockWidget *MainWindow::addReplayWidget(QMainWindow *parent)
{
    return makeDock(tr("Replay"), QStringLiteral("dock_replay"),
                    new ReplayWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addLinControlWidget(QMainWindow *parent)
{
    return makeDock(tr("LIN Control"), QStringLiteral("dock_lin_control"),
                    new LinControlWindow(nullptr, backend()), parent);
}

QDockWidget *MainWindow::addGpioControlWidget(QMainWindow *parent)
{
    return makeDock(tr("GPIO Control"), QStringLiteral("dock_gpio_control"),
                    new GpioControlWindow(nullptr, backend()), parent);
}

void MainWindow::setupDockFloatReparent(QDockWidget *dock, QMainWindow *innerParent)
{
    (void)innerParent;
    connect(dock, &QDockWidget::topLevelChanged, this, [dock](bool floating)
    {
        if (floating)
        {
            // Deferred so we don't destroy the window handle mid-drag.
            QTimer::singleShot(0, dock, [dock]()
            {
                dock->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint
                                     | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
                dock->show();
            });
        }
    });
}

void MainWindow::on_actionCan_Status_View_triggered()
{
    addStatusWidget();
}

bool MainWindow::showSetupDialog()
{
    MeasurementSetup new_setup(&backend());
    new_setup.cloneFrom(backend().getSetup());
    backend().setDefaultSetup();
    if (backend().getSetup().countNetworks() == new_setup.countNetworks())
        backend().setSetup(new_setup);
    else
        new_setup.cloneFrom(backend().getSetup());

#if defined(__linux__)
    // Default SocketCAN interfaces to "configured by OS" when not running as root.
    if (geteuid() != 0)
    {
        for (auto *network : new_setup.getNetworks())
        {
            for (auto *mi : network->interfaces())
            {
                BusInterface *intf = backend().getInterfaceById(mi->busInterface());
                if (intf && intf->getDriver()->getName() == "SocketCAN")
                    mi->setDoConfigure(false);
            }
        }
    }
#endif

    if (_setupDlg->showSetupDialog(new_setup))
    {
        if (!_setupDlg->isReflashNetworks())
            backend().setSetup(new_setup);

        setWorkspaceModified(true);
        _hasConfirmedSetup = true;
        return true;
    }
    return false;
}

void MainWindow::reloadInterfaces()
{
    backend().refreshInterfaces();
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this,
                       tr("About CANgaroo"),
                       "CANgaroo\n"
                       "Open Source CAN bus analyzer\n"
                       "https://github.com/Schildkroet/CANgaroo"
                       "\n"
                       "Version " VERSION_STRING "\n"
                       "\n"
                       "(c)2024-2026 Schildkroet\n"
                       "(c)2015-2017 Hubert Denkmair\n"
                       "(c)2018-2022 Ethan Zonca\n"
                       "(c)2024 WeAct Studio\n"
                       "(c)2025 Wikilift\n"
                       "(c)2026 Jayachandran Dharuman"
                       "\n\n"
                       "CANgaroo is free software licensed"
                       "\nunder the GPL v2 license.");
}

void MainWindow::startMeasurement()
{
    if (!_hasConfirmedSetup)
    {
        if (showSetupDialog())
        {
            backend().clearTrace();
            backend().startMeasurement();
        }
    }
    else
    {
        if (settings.value("ui/clearTraceOnStart", true).toBool())
            backend().clearTrace();
        backend().startMeasurement();
    }
}

void MainWindow::stopMeasurement()
{
    backend().stopMeasurement();
    for (auto *gen : findChildren<TxGeneratorWindow *>())
        gen->stopAll();
}

void MainWindow::saveTraceToFile()
{
    const QString filters("Vector ASC (*.asc);;Vector MDF4 (*.mf4);;Linux candump (*.candump);;PCAP (*.pcap);;PCAPng (*.pcapng)");
    const QString defaultFilter = settings.value("ui/preferredSaveFormat", "Vector ASC (*.asc)").toString();

    QFileDialog fileDialog(nullptr, tr("Save Trace to file"), QDir::currentPath(), filters);
    fileDialog.setAcceptMode(QFileDialog::AcceptSave);
    fileDialog.setOption(QFileDialog::DontConfirmOverwrite, false);
    fileDialog.selectNameFilter(defaultFilter);
    if (!fileDialog.exec())
        return;

    QString filename = fileDialog.selectedFiles().at(0);

    // If the user typed a name without extension, derive it from the selected filter.
    if (!filename.contains('.'))
    {
        QRegularExpression extRe("\\*(\\.\\w+)");
        QRegularExpressionMatch match = extRe.match(fileDialog.selectedNameFilter());
        filename += match.hasMatch() ? match.captured(1) : ".asc";
    }

    QFile file(filename);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate))
    {
        QMessageBox::warning(this, tr("Error"), tr("Cannot open file for writing."));
        return;
    }

    // Unrecognised extensions fall back to ASC, matching the dialog's default
    // filter. The scripting API shares this mapping but reports an error instead
    // of guessing (see cangaroo.save_trace).
    const TraceFileFormat format =
        traceFormatFromPath(filename).value_or(TraceFileFormat::VectorAsc);
    backend().getTrace()->save(file, format);

    file.close();
}

void MainWindow::on_action_TraceClear_triggered()
{
    backend().clearTrace();
    backend().clearLog();
}

void MainWindow::on_action_WorkspaceSave_triggered()
{
    saveWorkspace();
}

void MainWindow::on_action_WorkspaceSaveAs_triggered()
{
    saveWorkspaceAs();
}

void MainWindow::on_action_WorkspaceOpen_triggered()
{
    loadWorkspace();
}

void MainWindow::on_action_WorkspaceNew_triggered()
{
    newWorkspace();
}

void MainWindow::switchLanguage(QAction *action)
{
    const QString locale = action->data().toString();

    qApp->removeTranslator(&m_translator);

    if (locale == "en_US")
        std::ignore = m_translator.load("");
    else
    {
        const QString qmPath = ":/translations/i18n_" + locale + ".qm";
        if (!m_translator.load(qmPath))
            qDebug() << "Could not load translation: " << qmPath;
    }

    qApp->installTranslator(&m_translator);
    settings.setValue("ui/language", locale);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        ui->retranslateUi(this);
        _baseWindowTitle = tr("CANgaroo");
        setWorkspaceModified(_workspaceModified);
    }

    QMainWindow::changeEvent(event);
}

void MainWindow::createLanguageMenu()
{
    struct LangEntry { const char *label; const char *locale; };
    static constexpr LangEntry langs[] = {
        { QT_TR_NOOP("English"), "en_US" },
        { QT_TR_NOOP("Español"), "es_ES" },
        { QT_TR_NOOP("Deutsch"), "de_DE" },
        { QT_TR_NOOP("Chinese"), "zh_cn" },
    };

    m_languageActionGroup = new QActionGroup(this);
    connect(m_languageActionGroup, &QActionGroup::triggered, this, &MainWindow::switchLanguage);

    const QString savedLocale = settings.value("ui/language", "en_US").toString();

    for (const auto &lang : langs)
    {
        auto *action = new QAction(tr(lang.label), this);
        action->setCheckable(true);
        action->setData(QLatin1String(lang.locale));
        m_languageActionGroup->addAction(action);

        if (QLatin1String(lang.locale) == savedLocale)
        {
            action->setChecked(true);
            if (savedLocale != QLatin1String("en_US"))
                switchLanguage(action);
        }
    }
}

void MainWindow::exportFullTrace()
{
    QMessageBox::information(this, tr("Not Implemented"),
                             tr("Export full trace is not yet implemented."));
}

void MainWindow::importFullTrace()
{
    QMessageBox::information(this, tr("Not Implemented"),
                             tr("Import full trace is not yet implemented."));
}

void MainWindow::showSettingsDialog()
{
    SettingsDialog dlg(settings, m_languageActionGroup, this);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // Apply theme.
    const QString newTheme = dlg.selectedTheme();
    const QString currentTheme = QApplication::style()->objectName();
    const bool styleChanged = newTheme.compare(currentTheme, Qt::CaseInsensitive) != 0;

    const bool newNativeStyling = dlg.nativeStylingEnabled();
    const bool nativeStylingChanged = newNativeStyling != settings.value("ui/nativeStyling", true).toBool();

    if (styleChanged)
    {
        QApplication::setStyle(QStyleFactory::create(newTheme));
        settings.setValue("ui/applicationStyle", newTheme);
    }
    settings.setValue("ui/nativeStyling", newNativeStyling);

    if (styleChanged || nativeStylingChanged)
        applyCurrentTheme();

    // Apply language.
    const QString newLocale = dlg.selectedLanguage();
    const QString currentLocale = settings.value("ui/language", "en_US").toString();
    if (newLocale != currentLocale)
    {
        for (QAction *action : m_languageActionGroup->actions())
        {
            if (action->data().toString() == newLocale)
            {
                action->setChecked(true);
                switchLanguage(action);
                break;
            }
        }
    }

    // Apply font size.
    const int newFontSize = dlg.selectedFontSize();
    if (newFontSize != QApplication::font().pointSize())
    {
        applyFontSize(newFontSize);
        settings.setValue("ui/fontSize", newFontSize);
    }

    // Apply restore window setting.
    ui->actionRestore_Window->setChecked(dlg.restoreWindowEnabled());
    settings.setValue("ui/restoreWindowGeometry", dlg.restoreWindowEnabled());

    // Apply clear trace on start setting.
    settings.setValue("ui/clearTraceOnStart", dlg.clearTraceOnStart());

    // Apply UDS 29-bit decoding setting.
    settings.setValue("decoder/uds29Bit", dlg.uds29BitEnabled());
    backend().notifyDecoderConfigChanged();

    // Apply skip-save-prompt setting.
    settings.setValue("ui/skipSaveWorkspacePrompt", dlg.skipSaveWorkspacePrompt());

    // Apply preferred save format.
    settings.setValue("ui/preferredSaveFormat", dlg.preferredSaveFormat());

    // Apply trace window defaults.
    settings.setValue("tracewindow/defaultViewMode",      dlg.defaultTraceViewMode());
    settings.setValue("tracewindow/defaultTimestampMode", dlg.defaultTimestampMode());

    // Apply Data column display mode (hex/ASCII) -- takes effect immediately
    // on already-open trace windows, not just new ones.
    settings.setValue("tracewindow/dataAsciiMode", dlg.dataAsciiModeEnabled());
    backend().notifyDisplayConfigChanged();

    // Apply trace size limit.
    settings.setValue("trace/maxSize", dlg.maxTraceSize());
    backend().getTrace()->setMaxSize(dlg.maxTraceSize());
}

#if defined(_WIN32)
#ifdef ZSCANFD_DRIVER
void MainWindow::checkZsCanFdDlls()
{
    if (settings.value("mainWindow/zscanfdDllPromptDisabled", false).toBool())
        return;

    const QString appDir = QCoreApplication::applicationDirPath();
    if (QFile::exists(appDir + "/zscanfd.dll") &&
        QFile::exists(appDir + "/canbus/qtzscanfdbus.dll"))
        return;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("ZsCanFd DLLs not found"));
    msgBox.setText(tr("The required ZsCanFd DLLs (zilogic.com) "
                      "were not found (Optional driver).\n\n"
                      "Would you like to download them now?"));
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);

    auto *dontAsk = new QCheckBox(tr("Don't ask again"), &msgBox);
    msgBox.setCheckBox(dontAsk);

    const int result = msgBox.exec();

    if (dontAsk->isChecked())
        settings.setValue("mainWindow/zscanfdDllPromptDisabled", true);

    if (result == QMessageBox::Yes)
        downloadZsCanFdDlls();
}

void MainWindow::downloadZsCanFdDlls()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString tmpDir = QDir::tempPath() + "/cangaroo_zscanfd";
    QDir().mkpath(tmpDir);

    constexpr auto zscanfdUrl =
        "https://files-accl.zohoexternal.in/public/workdrive-external/download/"
        "vg4j6fe5343da544d4d78876807400716c283?x-cli-msg=%7B%22linkId%22%3A%22BHvzgCTCwc-13uP6TI%22"
        "%2C%22isFileOwner%22%3Afalse%2C%22version%22%3A%221.0%22%2C%22isWDSupport%22%3Afalse%7D";
    constexpr auto qtPluginUrl =
        "https://files-accl.zohoexternal.in/public/workdrive-external/download/"
        "5twqz7da53b7da5804c958ca079ba061bd493?x-cli-msg=%7B%22linkId%22%3A%22BHvzgCTCw7-13uP6TI%22"
        "%2C%22isFileOwner%22%3Afalse%2C%22version%22%3A%221.0%22%2C%22isWDSupport%22%3Afalse%7D";

    QNetworkAccessManager nam;

    auto downloadFile = [&](const char *url, const QString &destPath) -> bool
    {
        QProgressDialog progress(tr("Downloading ZsCanFd DLLs…"), tr("Cancel"), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.show();

        QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(QString::fromLatin1(url))));
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(reply, &QNetworkReply::downloadProgress,
                [&](qint64 received, qint64 total)
                {
                    if (total > 0)
                    {
                        progress.setMaximum(100);
                        progress.setValue(static_cast<int>(received * 100 / total));
                    }
                    QCoreApplication::processEvents();
                });
        connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError)
        {
            QMessageBox::warning(this, tr("Download failed"), reply->errorString());
            reply->deleteLater();
            return false;
        }

        QFile file(destPath);
        if (!file.open(QIODevice::WriteOnly))
        {
            QMessageBox::warning(this, tr("Download failed"),
                                 tr("Cannot write to: %1").arg(destPath));
            reply->deleteLater();
            return false;
        }
        file.write(reply->readAll());
        reply->deleteLater();
        return true;
    };

    auto extractZip = [&](const QString &zipPath, const QString &outDir) -> bool
    {
        QProcess ps;
        ps.start("powershell",
                 {"-Command",
                  QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
                      .arg(zipPath, outDir)});
        if (!ps.waitForFinished(60000) || ps.exitCode() != 0)
        {
            QMessageBox::warning(this, tr("Extraction failed"),
                                 tr("Failed to extract %1").arg(zipPath));
            return false;
        }
        return true;
    };

    const QString zip1 = tmpDir + "/zscanfd-package.zip";
    if (!downloadFile(zscanfdUrl, zip1)) return;
    if (!extractZip(zip1, tmpDir)) return;
    QFile::remove(appDir + "/zscanfd.dll");
    if (!QFile::copy(tmpDir + "/zscanfd-package/release/zscanfd.dll", appDir + "/zscanfd.dll"))
    {
        QMessageBox::warning(this, tr("Install failed"),
                             tr("Could not copy zscanfd.dll to application directory."));
        return;
    }

    const QString zip2 = tmpDir + "/qt-zscanfd.zip";
    if (!downloadFile(qtPluginUrl, zip2)) return;
    if (!extractZip(zip2, tmpDir)) return;
    QDir().mkpath(appDir + "/canbus");
    QFile::remove(appDir + "/canbus/qtzscanfdbus.dll");
    if (!QFile::copy(tmpDir + "/qtzscanfdbus.dll", appDir + "/canbus/qtzscanfdbus.dll"))
    {
        QMessageBox::warning(this, tr("Install failed"),
                             tr("Could not copy qtzscanfdbus.dll to canbus directory."));
        return;
    }

    QMessageBox::information(this, tr("Download complete"),
                             tr("ZsCanFd DLLs installed successfully.\n"
                                "Please restart CANgaroo to use ZsCanFd devices."));
}
#endif
#endif

void MainWindow::applyFontSize(int pointSize)
{
    QFont font = QApplication::font();
    font.setPointSize(pointSize);
    QApplication::setFont(font);

    for (QWidget *w : QApplication::allWidgets())
    {
        QFont wf = w->font();
        wf.setPointSize(pointSize);
        w->setFont(wf);
    }
}
