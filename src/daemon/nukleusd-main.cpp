#include "ChildProcessJob.h"
#include "LogFile.h"
#include "NukleusDaemon.h"
#include "ServiceHost.h"
#include <nukleus/ServiceProtocol.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>

// Exit codes, also the service-specific exit codes the Service Control Manager shows after a failed start.
constexpr int StartFailed = 1;
constexpr int InvalidOptions = 2;

int main(int argc, char *argv[])
{
    endChildrenWithThisProcess();
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("Nukleus"));
    application.setApplicationName(QStringLiteral("Nukleus"));
    NukleusService::ServiceHost host(QStringLiteral("nukleus"));
    host.startPending();
    QString logError;
    if (!NukleusService::installLogFileFromEnvironment(&logError)) NukleusService::report(QtWarningMsg, logError);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Nukleus model and job service"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("listen"), QStringLiteral("Optional LAN IPv4 address; 127.0.0.1 is always also bound (default: 127.0.0.1)."), QStringLiteral("address"), QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("WebSocket port."), QStringLiteral("port"), QString::number(Nukleus::Protocol::DefaultPort)});
    parser.addOption({QStringLiteral("token-file"), QStringLiteral("File containing the shared LAN token."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("models-dir"), QStringLiteral("Model weight directory; requires --config-dir (default: $XDG_DATA_HOME/nukleus/models)."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("config-dir"), QStringLiteral("Configuration directory holding the model catalogue in catalog/; requires --models-dir."), QStringLiteral("path")});
    // process() exits the program on a bad option, which the Service Control Manager would see as a crash.
    if (host.isService()) {
        if (!parser.parse(application.arguments())) { NukleusService::report(QtCriticalMsg, parser.errorText()); return host.failed(InvalidOptions); }
    } else {
        parser.process(application);
    }
    NukleusConfiguration config;
    const QString modelsDir = parser.value(QStringLiteral("models-dir")); const QString configDir = parser.value(QStringLiteral("config-dir"));
    if (modelsDir.isEmpty() != configDir.isEmpty()) { NukleusService::report(QtCriticalMsg, QStringLiteral("--models-dir and --config-dir must be given together")); return host.failed(InvalidOptions); }
    if (!modelsDir.isEmpty()) { config.modelRootPath = QDir(modelsDir).absolutePath(); config.catalogRootPath = QDir(QDir(configDir).absolutePath()).filePath(QStringLiteral("catalog")); }
    if (!config.listenAddress.setAddress(parser.value(QStringLiteral("listen")))) { NukleusService::report(QtCriticalMsg, QStringLiteral("Invalid listen address")); return host.failed(InvalidOptions); }
    bool ok = false; const int port = parser.value(QStringLiteral("port")).toInt(&ok); if (!ok || port < 1 || port > 65535) { NukleusService::report(QtCriticalMsg, QStringLiteral("Invalid port")); return host.failed(InvalidOptions); }
    config.port = static_cast<quint16>(port);
    const QString tokenPath = parser.value(QStringLiteral("token-file"));
    if (!tokenPath.isEmpty()) { QFile tokenFile(tokenPath); if (!tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) { NukleusService::report(QtCriticalMsg, QStringLiteral("Could not read token file")); return host.failed(InvalidOptions); } config.token = QString::fromUtf8(tokenFile.readAll()).trimmed(); }
    int exitCode = 0;
    {
        // Its end, with the event loop's, closes the listeners and the running job's processes.
        NukleusDaemon service(config); QString error;
        if (!service.start(&error)) { NukleusService::report(QtCriticalMsg, error); return host.failed(StartFailed); }
        QString listening = QStringLiteral("Nukleus listening on 127.0.0.1:%1").arg(config.port);
        if (!config.listenAddress.isLoopback()) listening += QStringLiteral(" and %1:%2").arg(config.listenAddress.toString()).arg(config.port);
        NukleusService::report(QtInfoMsg, listening);
        host.running();
        exitCode = application.exec();
    }
    host.stopped();
    return exitCode;
}
