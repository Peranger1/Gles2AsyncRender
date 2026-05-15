#pragma once

#include "work_scheduler.h"

#include <QHash>
#include <QMutex>
#include <QQueue>

#include <memory>

class IRequestCoalescer;

class SerialConflatedWorkScheduler final : public IWorkScheduler
{
public:
    explicit SerialConflatedWorkScheduler(std::shared_ptr<IRequestCoalescer> coalescer = nullptr);

    void submit(const WorkEnvelope &work) override;
    bool takeNext(WorkEnvelope *work) override;
    bool activeWork(WorkEnvelope *work) const override;
    void markActiveFinished(const WorkEnvelope &work, const ProcessorOutput *output) override;
    bool takePublication(WorkEnvelope *work, ProcessorOutput *output) override;
    void markPublicationDeferred(const WorkEnvelope &work) override;
    void markPublicationFinished(const WorkEnvelope &work) override;
    bool hasPending() const override;
    bool hasPublicationPending() const override;
    bool isPublishing() const override;
    bool hasPendingForLane(const LaneId &laneId) const override;
    bool snapshotLane(const LaneId &laneId, LaneSnapshot *snapshot) const override;
    void clear() override;

private:
    struct PublicationEntry final
    {
        WorkEnvelope work;
        ProcessorOutput output;
    };

    LaneId normalizedLaneId(const WorkEnvelope &work) const;
    LaneId normalizedLaneId(const LaneId &laneId) const;

    mutable QMutex m_mutex;
    QHash<LaneId, WorkEnvelope> m_activeByLane;
    QHash<LaneId, WorkEnvelope> m_pendingByLane;
    QHash<LaneId, PublicationEntry> m_publicationByLane;
    QQueue<LaneId> m_readyLaneOrder;
    QQueue<LaneId> m_readyPublicationOrder;
    LaneId m_publishingLaneId;
    std::shared_ptr<IRequestCoalescer> m_coalescer;
};
