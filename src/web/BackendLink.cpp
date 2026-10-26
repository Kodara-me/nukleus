#include "BackendLink.h"
#include "HostRules.h"

#include <QDebug>
#include <QUrl>

namespace NukleusWeb
{
BackendLink::BackendLink(const WebConfig &config, QObject *parent)
    : QObject(parent)
    , m_client(this)
{
    QUrl endpoint;
    endpoint.setScheme(QStringLiteral("ws"));
    endpoint.setHost(config.backendHost);
    endpoint.setPort(config.backendPort);
    m_client.setConfiguration(endpoint, tokenForBackendHost(config.backendHost, config.backendToken));
    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(config.reconnectIntervalMs);
    connect(&m_reconnect, &QTimer::timeout, &m_client, &Nukleus::ServiceClient::connectToService);
    connect(&m_client, &Nukleus::ServiceClient::serviceStateChanged, this,
            [this](Nukleus::ServiceConnectionState state, const QString &code, const QString &) { onServiceState(state, code); });
}

void BackendLink::start() { m_client.connectToService(); }

QString BackendLink::reasonForCode(const QString &code)
{
    if (code == QLatin1String("authentication-failed")) return QStringLiteral("authentication-refused");
    if (code.contains(QLatin1String("protocol")) || code.contains(QLatin1String("version")))
        return QStringLiteral("incompatible-protocol");
    return QStringLiteral("unreachable");
}

void BackendLink::onServiceState(Nukleus::ServiceConnectionState state, const QString &code)
{
    using S = Nukleus::ServiceConnectionState;
    switch (state) {
    case S::Connecting:
        m_reconnect.stop();
        setState(State::Connecting, {});
        break;
    case S::Authenticated:
    case S::Ready:
        m_reconnect.stop();
        setState(State::Available, {});
        break;
    case S::Error:
        if (m_client.isAuthenticated()) break; // a refused request, not a lost connection
        [[fallthrough]];
    case S::Disconnected:
        setState(State::Unavailable, reasonForCode(code));
        if (!m_reconnect.isActive()) m_reconnect.start();
        break;
    }
}

void BackendLink::setState(State state, const QString &reason)
{
    if (state == m_state && reason == m_reason) return;
    m_state = state;
    m_reason = reason;
    if (state == State::Unavailable)
        qInfo().noquote() << QStringLiteral("[Nukleus Web] Nukleus is unavailable: %1.").arg(reason);
    else if (state == State::Available)
        qInfo().noquote() << QStringLiteral("[Nukleus Web] Connected to Nukleus.");
    emit stateChanged();
}
}
