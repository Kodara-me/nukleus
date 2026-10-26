#pragma once
#include <QHostAddress>
#include <QString>
#include <QStringList>

namespace NukleusWeb
{
// True for a source address the service answers: loopback, private (10.0.0.0/8, 172.16.0.0/12,
// 192.168.0.0/16) or IPv4 link-local (169.254.0.0/16). IPv4-mapped IPv6 addresses count as their IPv4 form.
bool isAllowedSource(const QHostAddress &address);

// True when the backend host name or address designates this machine's loopback.
bool isLoopbackHost(const QString &host);

// The token to present to `nukleusd` at this host: none for loopback, the configured one otherwise.
QString tokenForBackendHost(const QString &host, const QString &configuredToken);

// The `Host` header values accepted on `port`, lower case: 127.0.0.1, localhost, the LAN address
// when one is configured, the host's name, its first label and that label followed by `.local`.
QStringList acceptedHostValues(const QString &hostName, const QHostAddress &lanAddress, quint16 port);
}
