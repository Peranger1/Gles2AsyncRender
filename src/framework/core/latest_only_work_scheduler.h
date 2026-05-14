#pragma once

#include "work_scheduler.h"

#include <QMutex>

class LatestOnlyWorkScheduler final : public IWorkScheduler
{
public:
    void submit(const WorkEnvelope &work) override;
    bool takeNext(WorkEnvelope *work) override;
    bool hasPending() const override;
    void cancel(quint64 workId) override;
    void clear() override;

private:
    mutable QMutex m_mutex;
    WorkEnvelope m_latest;
    bool m_hasLatest = false;
};
