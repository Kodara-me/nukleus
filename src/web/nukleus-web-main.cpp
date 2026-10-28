#include "LogFile.h"
#include "ServiceHost.h"
#include "WebConfig.h"
#include "WebService.h"

#include <QCoreApplication>
#include <QProcessEnvironment>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nukleus-web"));
    NukleusService::ServiceHost host(QStringLiteral("nukleus-web"));
    host.startPending();
    QString error;
    if (!NukleusService::installLogFileFromEnvironment(&error)) NukleusService::report(QtWarningMsg, QStringLiteral("nukleus-web: %1").arg(error));

    NukleusWeb::WebConfig config;
    if (!NukleusWeb::WebConfig::fromEnvironment(QProcessEnvironment::systemEnvironment(), &config, &error)) {
        NukleusService::report(QtCriticalMsg, QStringLiteral("nukleus-web: %1").arg(error));
        return host.failed(1);
    }
    int exitCode = 0;
    {
        // Its end, with the event loop's, closes the HTTP server and the connection to Nukleus.
        NukleusWeb::WebService service(config);
        if (!service.start(&error)) {
            NukleusService::report(QtCriticalMsg, QStringLiteral("nukleus-web: %1").arg(error));
            return host.failed(1);
        }
        host.running();
        exitCode = app.exec();
    }
    host.stopped();
    return exitCode;
}
