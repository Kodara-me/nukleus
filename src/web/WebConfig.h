#pragma once
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QString>

namespace NukleusWeb
{
struct WebConfig
{
    QHostAddress listenAddress;           // LAN IPv4 address besides 127.0.0.1, null for loopback only
    quint16 port = 9385;
    QString hostName;                     // the host's name for the `Host` check
    QString backendHost = QStringLiteral("127.0.0.1");
    quint16 backendPort = 9384;
    QString backendToken;                 // the LAN token, read from its file for a non-loopback host only
    int idleTimeoutMs = 60000;
    int reconnectIntervalMs = 5000;

    // Reads NUKLEUS_WEB_LISTEN, NUKLEUS_WEB_BACKEND_HOST and NUKLEUS_WEB_BACKEND_TOKEN_FILE.
    // On failure, returns false with a message that never holds the token.
    static bool fromEnvironment(const QProcessEnvironment &environment, WebConfig *config, QString *error);
};
}
