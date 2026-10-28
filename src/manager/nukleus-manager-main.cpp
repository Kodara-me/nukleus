#include "ManageModelsWindow.h"

#include "ManagerCli.h"
#include "ManagerSettings.h"

#include <QApplication>
#include <Ardoise/Theme.h>
#include <QIcon>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include <memory>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
// Started on its own, from a shortcut or Explorer, the manager gets a console window that would stay open
// beside the GUI; started from a shell, it shares that shell's console and keeps it for its messages.
void releaseOwnConsole()
{
#ifdef Q_OS_WIN
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1) FreeConsole();
#endif
}

bool hasArgument(int argc, char *argv[], const QString &argument)
{
    for (int index = 1; index < argc; ++index) if (QString::fromLocal8Bit(argv[index]) == argument) return true;
    return false;
}
}

int main(int argc, char *argv[])
{
    const bool guiRequested = hasArgument(argc, argv, QStringLiteral("--gui"));
    std::unique_ptr<QCoreApplication> application;
    if (guiRequested) releaseOwnConsole();
    if (guiRequested) application = std::make_unique<QApplication>(argc, argv);
    else application = std::make_unique<QCoreApplication>(argc, argv);
    application->setOrganizationName(QStringLiteral("Nukleus"));
    application->setApplicationName(QStringLiteral("Nukleus Manager"));
    ManagerSettings::migrateLegacySettings();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Manage models installed by the local Nukleus service."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("gui"), QStringLiteral("Start the graphical manager.")});
    ManagerCli::addOptions(parser);
    parser.process(*application);

    if (guiRequested) {
        if (parser.isSet("display-name")) { QTextStream(stderr) << "--gui cannot be combined with --display-name.\n"; return 2; }
        QStringList cliOptions = parser.optionNames();
        cliOptions.removeAll(QStringLiteral("gui"));
        if (!parser.positionalArguments().isEmpty() || !cliOptions.isEmpty()) {
            QTextStream(stderr) << "--gui cannot be combined with a CLI command.\n";
            return 2;
        }
        Ardoise::Theme::apply(*static_cast<QApplication *>(application.get()));
        QIcon icon;
        for (const int size : {16, 32, 48, 64, 128, 256, 512, 1024}) icon.addFile(QStringLiteral(":/manager/icons/manager-%1.png").arg(size));
        QApplication::setWindowIcon(icon);
        // Matches the installed nukleus-manager.desktop so Wayland shells show its icon.
        QGuiApplication::setDesktopFileName(QStringLiteral("nukleus-manager"));
        auto *window = new ManageModelsWindow;
        window->show();
        return application->exec();
    }
    if (parser.positionalArguments().isEmpty()) parser.showHelp(2);
    return ManagerCli::run(parser);
}
