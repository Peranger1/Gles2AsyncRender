#pragma once

#include "request_merge_strategy.h"
#include "request_queue_policy.h"

#include <memory>

class MergeWhileBusyPolicy final : public IRequestQueuePolicy
{
public:
    explicit MergeWhileBusyPolicy(std::shared_ptr<IRequestMergeStrategy> mergeStrategy = {})
        : m_mergeStrategy(mergeStrategy ? std::move(mergeStrategy)
                                        : std::make_shared<ReplaceWithLatestMergeStrategy>())
    {
    }

    bool submitWhileBusy(const ExecutionRequest &incoming, QString *error) override
    {
        if (!m_waiting.has_value()) {
            m_waiting = incoming;
            return true;
        }

        if (!m_mergeStrategy || !m_mergeStrategy->canMerge(*m_waiting, incoming)) {
            if (error) {
                *error = QStringLiteral("MergeWhileBusyPolicy rejected a busy request because no merge rule matched.");
            }
            return false;
        }

        ExecutionRequest merged;
        const MergeDisposition disposition = m_mergeStrategy->merge(*m_waiting, incoming, &merged, error);
        if (disposition == MergeDisposition::ReplaceWaiting) {
            m_waiting = merged;
            return true;
        }
        if (disposition == MergeDisposition::KeepExistingWaiting) {
            return true;
        }
        return false;
    }

    bool hasWaiting() const override
    {
        return m_waiting.has_value();
    }

    std::optional<ExecutionRequest> takeNext() override
    {
        const std::optional<ExecutionRequest> next = m_waiting;
        m_waiting.reset();
        return next;
    }

    QueueSnapshot snapshot() const override
    {
        QueueSnapshot snapshot;
        snapshot.hasWaiting = m_waiting.has_value();
        snapshot.waitingCount = snapshot.hasWaiting ? 1 : 0;
        return snapshot;
    }

    void clear() override
    {
        m_waiting.reset();
    }

private:
    std::shared_ptr<IRequestMergeStrategy> m_mergeStrategy;
    std::optional<ExecutionRequest> m_waiting;
};
