#include "KiraService.h"
#include "ServiceProtocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("Kira Canvas"));
    application.setApplicationName(QStringLiteral("Kira Service"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Kira Canvas image service"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("listen"), QStringLiteral("Optional LAN IPv4 address; 127.0.0.1 is always also bound (default: 127.0.0.1)."), QStringLiteral("address"), QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("WebSocket port."), QStringLiteral("port"), QString::number(ServiceProtocol::DefaultPort)});
    parser.addOption({QStringLiteral("token-file"), QStringLiteral("File containing the shared LAN token."), QStringLiteral("path")});
    parser.process(application);
    KiraServiceConfiguration config;
    if (!config.listenAddress.setAddress(parser.value(QStringLiteral("listen")))) { QTextStream(stderr) << "Invalid listen address\n"; return 2; }
    bool ok = false; const int port = parser.value(QStringLiteral("port")).toInt(&ok); if (!ok || port < 1 || port > 65535) { QTextStream(stderr) << "Invalid port\n"; return 2; }
    config.port = static_cast<quint16>(port);
    const QString tokenPath = parser.value(QStringLiteral("token-file"));
    if (!tokenPath.isEmpty()) { QFile tokenFile(tokenPath); if (!tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) { QTextStream(stderr) << "Could not read token file\n"; return 2; } config.token = QString::fromUtf8(tokenFile.readAll()).trimmed(); }
    KiraService service(config); QString error;
    if (!service.start(&error)) { QTextStream(stderr) << error << '\n'; return 1; }
    QTextStream output(stdout);
    output << "Kira Service listening on 127.0.0.1:" << config.port;
    if (!config.listenAddress.isLoopback()) output << " and " << config.listenAddress.toString() << ':' << config.port;
    output << '\n';
    return application.exec();
}
