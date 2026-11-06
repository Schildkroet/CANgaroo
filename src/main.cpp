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

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QStyleFactory>
#include <QTranslator>

#include <QCommandLineParser>

#if defined(__linux__)
// Returns true if a Qt plugin whose file name contains `needle` exists in the
// given plugin sub-directory (e.g. "platformthemes", "wayland-decoration-client").
static bool hasQtPlugin(const QString &subdir, const QString &needle)
{
    QDir dir(QLibraryInfo::path(QLibraryInfo::PluginsPath) + QLatin1Char('/') + subdir);
    const auto entries = dir.entryList(QDir::Files);
    return std::any_of(entries.begin(), entries.end(),
        [&](const QString &f) { return f.contains(needle, Qt::CaseInsensitive); });
}

// On a GNOME/Wayland session, default to a native Qt platform theme and (if
// available) native Adwaita window decorations so the app looks native out of
// the box. Only pick values whose plugin is actually installed, and only when
// the variable is unset, so user/packager overrides win and we never request a
// missing plugin (which would log a "decoration not found" warning).
// Must run before the QApplication is constructed (env is read during init).
static void setupNativeEnvironmentDefaults()
{
    const QString sessionType = qEnvironmentVariable("XDG_SESSION_TYPE");
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");

    const bool isWayland = sessionType.compare("wayland", Qt::CaseInsensitive) == 0;
    const bool isGnome = desktop.contains("GNOME", Qt::CaseInsensitive);

    if (!isWayland || !isGnome)
        return;

    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME"))
    {
        if (hasQtPlugin("platformthemes", "gnomeplatform"))
            qputenv("QT_QPA_PLATFORMTHEME", "gnome");
        else if (hasQtPlugin("platformthemes", "gtk3"))
            qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    }

    // Only request the Adwaita Qt decoration if its plugin is present; otherwise
    // leave it to Qt's default decoration / libdecor.
    if (!qEnvironmentVariableIsSet("QT_WAYLAND_DECORATION")
        && hasQtPlugin("wayland-decoration-client", "adwaita"))
    {
        qputenv("QT_WAYLAND_DECORATION", "adwaita");
    }
}
#endif


int main(int argc, char *argv[])
{
#if defined(__linux__)
    setupNativeEnvironmentDefaults();
#endif

    QApplication a(argc, argv);

    QCoreApplication::setOrganizationName("CANgaroo/Schildkroet");
    QCoreApplication::setOrganizationDomain("CANgaroo/Schildkroet");
    QCoreApplication::setApplicationName("CANgaroo");

    // Kommandozeilen-Parser
    QCommandLineParser parser;
    parser.setApplicationDescription("CANgaroo CAN Bus Analyzer");
    parser.addHelpOption();
    parser.addVersionOption();

    // Workspace-Option
    QCommandLineOption workspaceOption(
        QStringList() << "w" << "workspace",
        "Load workspace from file.",
        "file"
        );

    parser.addOption(workspaceOption);
    parser.process(a);

    MainWindow w;

    // load Workspace
    const QString workspaceFile = parser.value(workspaceOption);

    if (!workspaceFile.isEmpty())
    {
        QFileInfo fileInfo(workspaceFile);

        if (fileInfo.exists() && fileInfo.isFile())
        {
            w.loadWorkspaceFromFile(fileInfo.absoluteFilePath());
        }
        else
        {
            qWarning() << "Workspace file not found:" << workspaceFile;
        }
    }

    if(w.isMaximizedWindow())
    {
        w.showMaximized();
    }
    else
    {
        w.show();
    }
    return a.exec();
}
