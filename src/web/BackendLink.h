#pragma once
#include "WebConfig.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <nukleus/ServiceClient.h>

namespace NukleusWeb
{
// The web service's single connection to `nukleusd`: it tracks whether Nukleus is available and
// why not, and reconnects on its own.
class BackendLink final : public QObject
{
    Q_OBJECT
public:
    enum class State { Connecting, Available, Unavailable };

    explicit BackendLink(const WebConfig &config, QObject *parent = nullptr);

    void start();
    State state() const { return m_state; }
    // "unreachable", "authentication-refused" or "incompatible-protocol" while unavailable, empty otherwise.
    QString reason() const { return m_reason; }
    Nukleus::ServiceClient *client() { return &m_client; }
    const Nukleus::ServiceClient *client() const { return &m_client; }

    // Maps the SDK's error code to the reason the interface shows.
    static QString reasonForCode(const QString &code);

signals:
    void stateChanged();

private:
    void onServiceState(Nukleus::ServiceConnectionState state, const QString &code);
    void setState(State state, const QString &reason);

    Nukleus::ServiceClient m_client;
    QTimer m_reconnect;
    State m_state = State::Connecting;
    QString m_reason;
};
}
