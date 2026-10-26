#include "WebConfig.h"
#include "WebService.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nukleus-web"));

    NukleusWeb::WebConfig config;
    QString error;
    if (!NukleusWeb::WebConfig::fromEnvironment(QProcessEnvironment::systemEnvironment(), &config, &error)) {
        std::fprintf(stderr, "nukleus-web: %s\n", qPrintable(error));
        return 1;
    }
    NukleusWeb::WebService service(config);
    if (!service.start(&error)) {
        std::fprintf(stderr, "nukleus-web: %s\n", qPrintable(error));
        return 1;
    }
    return app.exec();
}
