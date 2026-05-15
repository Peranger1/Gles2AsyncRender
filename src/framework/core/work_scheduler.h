#pragma once

#include "work_types.h"

class IWorkScheduler
{
public:
    virtual ~IWorkScheduler() = default;

    virtual void submit(const WorkEnvelope &work) = 0;
    virtual bool takeNext(WorkEnvelope *work) = 0;
    virtual bool activeWork(WorkEnvelope *work) const = 0;
    virtual void markActiveFinished(const WorkEnvelope &work, const ProcessorOutput *output) = 0;
    virtual bool takePublication(WorkEnvelope *work, ProcessorOutput *output) = 0;
    virtual void markPublicationDeferred(const WorkEnvelope &work) = 0;
    virtual void markPublicationFinished(const WorkEnvelope &work) = 0;
    virtual bool hasPending() const = 0;
    virtual bool hasPublicationPending() const = 0;
    virtual bool isPublishing() const = 0;
    virtual bool hasPendingForLane(const LaneId &laneId) const = 0;
    virtual bool snapshotLane(const LaneId &laneId, LaneSnapshot *snapshot) const = 0;
    virtual void clear() = 0;
};
