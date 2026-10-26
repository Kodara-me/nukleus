#include "JobStore.h"

#include <QRandomGenerator>

namespace NukleusWeb
{
namespace
{
QString randomId()
{
    QByteArray bytes(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), 4);
    return QString::fromLatin1(bytes.toHex());
}
}

JobStore::JobStore(int capacity)
    : m_capacity(capacity)
{
}

WebJob &JobStore::add(const QString &sdkId)
{
    WebJob job;
    do job.id = randomId(); while (find(job.id));
    job.sdkId = sdkId;
    m_jobs.append(job);
    while (m_jobs.size() > m_capacity) m_jobs.removeFirst();
    return m_jobs.last();
}

bool JobStore::isWellFormedId(const QString &id)
{
    if (id.size() != 32) return false;
    for (const QChar c : id)
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) return false;
    return true;
}

WebJob *JobStore::find(const QString &id)
{
    if (!isWellFormedId(id)) return nullptr;
    for (WebJob &job : m_jobs)
        if (job.id == id) return &job;
    return nullptr;
}

WebJob *JobStore::findBySdkId(const QString &sdkId)
{
    for (WebJob &job : m_jobs)
        if (job.sdkId == sdkId) return &job;
    return nullptr;
}
}
