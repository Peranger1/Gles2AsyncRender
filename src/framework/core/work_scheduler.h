#pragma once

#include "work_types.h"

class IWorkScheduler
{
public:
    virtual ~IWorkScheduler() = default;

    virtual void submit(const WorkEnvelope &work) = 0;
    virtual bool takeNext(WorkEnvelope *work) = 0;
    virtual bool hasPending() const = 0;
    virtual void cancel(quint64 workId) = 0;
    virtual void clear() = 0;
};
