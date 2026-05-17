#pragma once

#include "execution_common.h"
#include "runtime_host.h"

#include <functional>
#include <memory>

template <typename Args, typename Result>
class SyncLane
{
public:
    using Runner = std::function<ExecutionOutcome<Result>(const TaskContext &, const Args &)>;

    SyncLane(RuntimeHost *host,
             LaneConfig config,
             Runner runner,
             std::shared_ptr<IWaitingMerger<Args>> merger = {})
        : m_host(host)
        , m_config(std::move(config))
        , m_runner(std::move(runner))
        , m_merger(std::move(merger))
    {
    }

    ExecutionOutcome<Result> invoke(const Args &args)
    {
        if (!m_host || !m_runner) {
            ExecutionError error;
            error.message = QStringLiteral("The sync lane is unavailable.");
            return ExecutionOutcome<Result>::failure(std::move(error));
        }

        ExecutionError initialError;
        ExecutionOutcome<Result> outcome = ExecutionOutcome<Result>::failure(std::move(initialError));
        QString errorText;
        const bool ok = m_host->dispatchSync([&](IRuntime *runtime) {
            TaskContext context;
            context.runtime = runtime;
            outcome = m_runner(context, args);
        }, &errorText);
        if (!ok) {
            ExecutionError error;
            error.message = errorText;
            return ExecutionOutcome<Result>::failure(std::move(error));
        }
        return outcome;
    }

    void shutdown()
    {
    }

private:
    RuntimeHost *m_host = nullptr;
    LaneConfig m_config;
    Runner m_runner;
    std::shared_ptr<IWaitingMerger<Args>> m_merger;
};
