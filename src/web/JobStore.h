#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace NukleusWeb
{
struct WebJob
{
    enum class State { Queued, Running, Finished, Failed };

    QString id;      // the identifier the browser knows: 128 random bits, hex
    QString sdkId;   // the SDK's job id, never sent to a browser
    State state = State::Queued;
    int queuePosition = -1; // -1 until Nukleus reports one
    int progress = -1;      // percentage while running, -1 when unknown
    QString error;
    QByteArray result;
    QByteArray mimeType;
};

// The most recent jobs of the web service, in memory only (R44, R45). An identifier the store no
// longer holds is a job that is no longer available.
class JobStore final
{
public:
    explicit JobStore(int capacity = 20);

    // Adds a job for an SDK job id and returns it; the oldest job beyond the capacity is dropped.
    WebJob &add(const QString &sdkId);
    // Null for an identifier that is not a well-formed identifier or that the store does not hold.
    WebJob *find(const QString &id);
    WebJob *findBySdkId(const QString &sdkId);
    int count() const { return m_jobs.size(); }

    static bool isWellFormedId(const QString &id);

private:
    int m_capacity;
    QList<WebJob> m_jobs; // oldest first
};
}
