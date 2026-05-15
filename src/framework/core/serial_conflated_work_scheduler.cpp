#include "serial_conflated_work_scheduler.h"

#include "replace_with_latest_coalescer.h"

#include <QMutexLocker>

namespace
{
const QString &defaultLaneId()
{
    static const QString kDefaultLaneId = QStringLiteral("__default_lane__");
    return kDefaultLaneId;
}
}

SerialConflatedWorkScheduler::SerialConflatedWorkScheduler(std::shared_ptr<IRequestCoalescer> coalescer)
    : m_coalescer(coalescer ? std::move(coalescer)
                            : std::make_shared<ReplaceWithLatestCoalescer>())
{
}

void SerialConflatedWorkScheduler::submit(const WorkEnvelope &work)
{
    QMutexLocker locker(&m_mutex);
    const LaneId laneId = normalizedLaneId(work);
    auto it = m_pendingByLane.find(laneId);
    if (it == m_pendingByLane.end()) {
        m_pendingByLane.insert(laneId, work);
        m_readyLaneOrder.enqueue(laneId);
        return;
    }

    WorkEnvelope resolvedPending = work;
    if (m_coalescer && m_coalescer->canCoalesce(it.value(), work)) {
        const MergeDisposition disposition = m_coalescer->merge(it.value(), work, &resolvedPending, nullptr);
        switch (disposition) {
        case MergeDisposition::KeepExistingPending:
            return;
        case MergeDisposition::ReplacePending:
        case MergeDisposition::MergeIntoPending:
            it.value() = resolvedPending;
            return;
        case MergeDisposition::RejectMerge:
        default:
            break;
        }
    }

    it.value() = work;
}

bool SerialConflatedWorkScheduler::takeNext(WorkEnvelope *work)
{
    QMutexLocker locker(&m_mutex);
    if (work == nullptr) {
        return false;
    }
    if (!m_activeByLane.isEmpty() || !m_publicationByLane.isEmpty() || !m_publishingLaneId.isEmpty()) {
        return false;
    }

    while (!m_readyLaneOrder.isEmpty()) {
        const LaneId laneId = m_readyLaneOrder.dequeue();
        auto it = m_pendingByLane.find(laneId);
        if (it == m_pendingByLane.end()) {
            continue;
        }

        *work = it.value();
        m_activeByLane.insert(laneId, *work);
        m_pendingByLane.erase(it);
        return true;
    }

    return false;
}

bool SerialConflatedWorkScheduler::activeWork(WorkEnvelope *work) const
{
    QMutexLocker locker(&m_mutex);
    if (work == nullptr || m_activeByLane.isEmpty()) {
        return false;
    }

    *work = m_activeByLane.constBegin().value();
    return true;
}

void SerialConflatedWorkScheduler::markActiveFinished(const WorkEnvelope &work, const ProcessorOutput *output)
{
    QMutexLocker locker(&m_mutex);
    const LaneId laneId = normalizedLaneId(work);
    auto it = m_activeByLane.find(laneId);
    if (it != m_activeByLane.end() && it.value().requestId == work.requestId) {
        const WorkEnvelope completedWork = it.value();
        m_activeByLane.erase(it);
        if (output != nullptr) {
            PublicationEntry entry;
            entry.work = completedWork;
            entry.output = *output;
            const bool isNewPublication = !m_publicationByLane.contains(laneId);
            m_publicationByLane.insert(laneId, entry);
            if (isNewPublication) {
                m_readyPublicationOrder.enqueue(laneId);
            }
        }
    }
}

bool SerialConflatedWorkScheduler::takePublication(WorkEnvelope *work, ProcessorOutput *output)
{
    QMutexLocker locker(&m_mutex);
    if (work == nullptr || output == nullptr || !m_publishingLaneId.isEmpty()) {
        return false;
    }

    while (!m_readyPublicationOrder.isEmpty()) {
        const LaneId laneId = m_readyPublicationOrder.dequeue();
        auto it = m_publicationByLane.find(laneId);
        if (it == m_publicationByLane.end()) {
            continue;
        }

        *work = it->work;
        *output = it->output;
        m_publishingLaneId = laneId;
        return true;
    }

    return false;
}

void SerialConflatedWorkScheduler::markPublicationDeferred(const WorkEnvelope &work)
{
    QMutexLocker locker(&m_mutex);
    const LaneId laneId = normalizedLaneId(work);
    if (m_publishingLaneId != laneId || !m_publicationByLane.contains(laneId)) {
        return;
    }

    m_publishingLaneId.clear();
    m_readyPublicationOrder.enqueue(laneId);
}

void SerialConflatedWorkScheduler::markPublicationFinished(const WorkEnvelope &work)
{
    QMutexLocker locker(&m_mutex);
    const LaneId laneId = normalizedLaneId(work);
    if (m_publishingLaneId == laneId) {
        m_publishingLaneId.clear();
    }

    auto it = m_publicationByLane.find(laneId);
    if (it != m_publicationByLane.end() && it->work.requestId == work.requestId) {
        m_publicationByLane.erase(it);
    }
}

bool SerialConflatedWorkScheduler::hasPending() const
{
    QMutexLocker locker(&m_mutex);
    return !m_pendingByLane.isEmpty();
}

bool SerialConflatedWorkScheduler::hasPublicationPending() const
{
    QMutexLocker locker(&m_mutex);
    return !m_publicationByLane.isEmpty();
}

bool SerialConflatedWorkScheduler::isPublishing() const
{
    QMutexLocker locker(&m_mutex);
    return !m_publishingLaneId.isEmpty();
}

bool SerialConflatedWorkScheduler::hasPendingForLane(const LaneId &laneId) const
{
    QMutexLocker locker(&m_mutex);
    return m_pendingByLane.contains(normalizedLaneId(laneId));
}

bool SerialConflatedWorkScheduler::snapshotLane(const LaneId &laneId, LaneSnapshot *snapshot) const
{
    if (snapshot == nullptr) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    *snapshot = {};
    const LaneId normalized = normalizedLaneId(laneId);
    const auto activeIt = m_activeByLane.constFind(normalized);
    if (activeIt != m_activeByLane.constEnd()) {
        snapshot->hasActive = true;
        snapshot->active = activeIt.value();
    }

    const auto pendingIt = m_pendingByLane.constFind(normalized);
    if (pendingIt != m_pendingByLane.constEnd()) {
        snapshot->hasPending = true;
        snapshot->pending = pendingIt.value();
    }

    const auto publicationIt = m_publicationByLane.constFind(normalized);
    if (publicationIt != m_publicationByLane.constEnd()) {
        snapshot->hasPublicationPending = true;
        snapshot->publication = publicationIt->work;
        snapshot->isPublishing = m_publishingLaneId == normalized;
    }

    return snapshot->hasActive || snapshot->hasPending || snapshot->hasPublicationPending;
}

void SerialConflatedWorkScheduler::clear()
{
    QMutexLocker locker(&m_mutex);
    m_activeByLane.clear();
    m_pendingByLane.clear();
    m_publicationByLane.clear();
    m_readyLaneOrder.clear();
    m_readyPublicationOrder.clear();
    m_publishingLaneId.clear();
}

LaneId SerialConflatedWorkScheduler::normalizedLaneId(const WorkEnvelope &work) const
{
    return normalizedLaneId(work.laneId);
}

LaneId SerialConflatedWorkScheduler::normalizedLaneId(const LaneId &laneId) const
{
    return laneId.isEmpty() ? defaultLaneId() : laneId;
}
