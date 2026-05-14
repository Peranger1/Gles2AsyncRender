#include "latest_only_work_scheduler.h"

#include <QMutexLocker>

void LatestOnlyWorkScheduler::submit(const WorkEnvelope &work)
{
    QMutexLocker locker(&m_mutex);
    m_latest = work;
    m_hasLatest = true;
}

bool LatestOnlyWorkScheduler::takeNext(WorkEnvelope *work)
{
    QMutexLocker locker(&m_mutex);
    if (!m_hasLatest || work == nullptr) {
        return false;
    }

    *work = m_latest;
    m_hasLatest = false;
    return true;
}

bool LatestOnlyWorkScheduler::hasPending() const
{
    QMutexLocker locker(&m_mutex);
    return m_hasLatest;
}

void LatestOnlyWorkScheduler::cancel(quint64 workId)
{
    QMutexLocker locker(&m_mutex);
    if (m_hasLatest && m_latest.workId == workId) {
        m_hasLatest = false;
        m_latest = {};
    }
}

void LatestOnlyWorkScheduler::clear()
{
    QMutexLocker locker(&m_mutex);
    m_hasLatest = false;
    m_latest = {};
}
