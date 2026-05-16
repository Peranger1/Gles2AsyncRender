#pragma once

#include "execution_types.h"

#include <optional>

struct QueueSnapshot final
{
    bool hasWaiting = false;
    int waitingCount = 0;
};

class IRequestQueuePolicy
{
public:
    virtual ~IRequestQueuePolicy() = default;

    virtual bool submitWhileBusy(const ExecutionRequest &incoming, QString *error) = 0;
    virtual bool hasWaiting() const = 0;
    virtual std::optional<ExecutionRequest> takeNext() = 0;
    virtual QueueSnapshot snapshot() const = 0;
    virtual void clear() = 0;
};
