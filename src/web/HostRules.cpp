#include "HostRules.h"

namespace NukleusWeb
{
bool isAllowedSource(const QHostAddress &address)
{
    QHostAddress candidate = address;
    if (candidate.protocol() == QAbstractSocket::IPv6Protocol) {
        if (candidate == QHostAddress::LocalHostIPv6) return true;
        bool ok = false;
        const quint32 mapped = candidate.toIPv4Address(&ok);
        if (!ok) return false;
        candidate = QHostAddress(mapped);
    }
    if (candidate.protocol() != QAbstractSocket::IPv4Protocol) return false;
    return candidate.isLoopback()
        || candidate.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)
        || candidate.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)
        || candidate.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)
        || candidate.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16);
}

bool isLoopbackHost(const QString &host)
{
    const QString name = host.trimmed().toLower();
    if (name == QLatin1String("localhost")) return true;
    QHostAddress address;
    return address.setAddress(name) && address.isLoopback();
}

QString tokenForBackendHost(const QString &host, const QString &configuredToken)
{
    return isLoopbackHost(host) ? QString() : configuredToken;
}

QStringList acceptedHostValues(const QString &hostName, const QHostAddress &lanAddress, quint16 port)
{
    QStringList names{QStringLiteral("127.0.0.1"), QStringLiteral("localhost")};
    if (!lanAddress.isNull()) names << lanAddress.toString();
    const QString name = hostName.trimmed().toLower();
    if (!name.isEmpty()) {
        const QString label = name.section(QLatin1Char('.'), 0, 0);
        names << name << label << label + QStringLiteral(".local");
    }
    QStringList values;
    for (const QString &candidate : std::as_const(names)) {
        const QString value = candidate + QLatin1Char(':') + QString::number(port);
        if (!values.contains(value)) values << value;
    }
    return values;
}
}
