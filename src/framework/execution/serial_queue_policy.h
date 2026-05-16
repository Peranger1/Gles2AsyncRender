#pragma once

#include "request_queue_policy.h"

#include <deque>

class SerialQueuePolicy final : public IRequestQueuePolicy
{
public:
    bool submitWhileBusy(const ExecutionRequest &incoming, QString *error) override
    {
        Q_UNUSED(error);
        m_waiting.push_back(incoming);
        return true;
    }

    bool hasWaiting() const override
    {
        return !m_waiting.empty();
    }

    std::optional<ExecutionRequest> takeNext() override
    {
        if (m_waiting.empty()) {
            return std::nullopt;
        }

        ExecutionRequest next = m_waiting.front();
        m_waiting.pop_front();
        return next;
    }

    QueueSnapshot snapshot() const override
    {
        QueueSnapshot snapshot;
        snapshot.hasWaiting = !m_waiting.empty();
        snapshot.waitingCount = int(m_waiting.size());
        return snapshot;
    }

    void clear() override
    {
        m_waiting.clear();
    }

private:
    std::deque<ExecutionRequest> m_waiting;
};
