#include "WebConfig.h"
#include "HostRules.h"

#include <QFile>
#include <QHostInfo>

namespace NukleusWeb
{
bool WebConfig::fromEnvironment(const QProcessEnvironment &environment, WebConfig *config, QString *error)
{
    WebConfig result;
    result.hostName = QHostInfo::localHostName();

    const QString listen = environment.value(QStringLiteral("NUKLEUS_WEB_LISTEN")).trimmed();
    if (!listen.isEmpty()) {
        QHostAddress address;
        if (!address.setAddress(listen) || address.protocol() != QAbstractSocket::IPv4Protocol
            || address.isNull() || address == QHostAddress(QHostAddress::AnyIPv4) || address.isMulticast()) {
            *error = QStringLiteral("NUKLEUS_WEB_LISTEN must be an IPv4 address of this host.");
            return false;
        }
        result.listenAddress = address;
    }

    const QString host = environment.value(QStringLiteral("NUKLEUS_WEB_BACKEND_HOST")).trimmed();
    if (!host.isEmpty()) result.backendHost = host;

    if (!isLoopbackHost(result.backendHost)) {
        const QString path = environment.value(QStringLiteral("NUKLEUS_WEB_BACKEND_TOKEN_FILE")).trimmed();
        if (path.isEmpty()) {
            *error = QStringLiteral("NUKLEUS_WEB_BACKEND_TOKEN_FILE is required for a backend host that is not loopback.");
            return false;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Cannot read the backend token file %1: %2").arg(path, file.errorString());
            return false;
        }
        result.backendToken = QString::fromUtf8(file.readAll()).trimmed();
        if (result.backendToken.isEmpty()) {
            *error = QStringLiteral("The backend token file %1 is empty.").arg(path);
            return false;
        }
    }
    *config = result;
    return true;
}
}
