#pragma once

#include "execution_common.h"
#include "runtime_host.h"

#include <QMutex>
#include <QMutexLocker>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

template <typename Args, typename Result>
class AsyncLane
{
public:
    using Completion = std::function<void(TaskId, ExecutionOutcome<Result>)>;
    using Done = std::function<void(ExecutionOutcome<Result>)>;
    using Starter = std::function<void(const TaskContext &, const Args &, Done)>;

    AsyncLane(RuntimeHost *host,
              LaneConfig config,
              Starter starter,
              std::shared_ptr<IWaitingMerger<Args>> merger = {})
        : m_host(host)
        , m_config(std::move(config))
        , m_starter(std::move(starter))
        , m_merger(std::move(merger))
    {
    }

    SubmitResult submit(const Args &args, Completion completion)
    {
        PendingTask task;
        task.id = ++m_nextTaskId;
        task.args = args;
        task.completion = std::move(completion);

        PendingTask activeToStart;
        bool shouldStart = false;
        SubmitResult result;
        result.accepted = false;
        result.taskId = task.id;

        {
            QMutexLocker locker(&m_mutex);
            if (m_shuttingDown || m_host == nullptr || !m_starter) {
                result.error.state = TaskState::Shutdown;
                result.error.message = QStringLiteral("The async lane is unavailable.");
                return result;
            }

            if (!m_active.has_value()) {
                m_active = task;
                activeToStart = *m_active;
                shouldStart = true;
                result.accepted = true;
            } else {
                switch (m_config.queuePolicy) {
                case QueuePolicyKind::Immediate:
                    result.error.state = TaskState::Rejected;
                    result.error.message = QStringLiteral("The async lane rejected the task because it is busy.");
                    return result;
                case QueuePolicyKind::Serial:
                    if (m_waiting.size() >= qMax(1, m_config.maxWaitingCount)) {
                        result.error.state = TaskState::Rejected;
                        result.error.message = QStringLiteral("The async lane waiting queue is full.");
                        return result;
                    }
                    m_waiting.push_back(task);
                    result.accepted = true;
                    break;
                case QueuePolicyKind::MergeWhileBusy:
                    if (m_waiting.empty()) {
                        m_waiting.push_back(task);
                        result.accepted = true;
                        break;
                    }

                    if (!m_merger || !m_merger->canMerge(m_waiting.back().args, task.args)) {
                        result.error.state = TaskState::Rejected;
                        result.error.message = QStringLiteral("The async lane could not merge the waiting task.");
                        return result;
                    }

                    PendingTask mergedTask = task;
                    mergedTask.args = m_merger->merge(m_waiting.back().args, task.args);
                    m_waiting.back() = mergedTask;
                    result.accepted = true;
                    return result;
                }
            }
        }

        if (shouldStart) {
            startTask(activeToStart);
        }
        return result;
    }

    void shutdown()
    {
        std::optional<PendingTask> active;
        std::deque<PendingTask> waiting;
        {
            QMutexLocker locker(&m_mutex);
            if (m_shuttingDown) {
                return;
            }
            m_shuttingDown = true;
            active = m_active;
            waiting = std::move(m_waiting);
            m_active.reset();
            m_waiting.clear();
        }

        const auto failTask = [](PendingTask &task) {
            if (!task.completion) {
                return;
            }
            ExecutionError error;
            error.state = TaskState::Shutdown;
            error.message = QStringLiteral("The async lane was shut down before the task completed.");
            task.completion(task.id, ExecutionOutcome<Result>::failure(std::move(error)));
        };

        if (active.has_value()) {
            failTask(*active);
        }
        for (PendingTask &task : waiting) {
            failTask(task);
        }
    }

    bool hasActiveTask() const
    {
        QMutexLocker locker(&m_mutex);
        return m_active.has_value();
    }

    bool hasWaitingTask() const
    {
        QMutexLocker locker(&m_mutex);
        return !m_waiting.empty();
    }

    int waitingCount() const
    {
        QMutexLocker locker(&m_mutex);
        return int(m_waiting.size());
    }

private:
    struct PendingTask final
    {
        TaskId id = 0;
        Args args;
        Completion completion;
    };

    void startTask(const PendingTask &task)
    {
        QString error;
        const bool dispatched = m_host->dispatchAsync([this, task](IRuntime *runtime) {
            TaskContext context;
            context.taskId = task.id;
            context.runtime = runtime;
            m_starter(context, task.args, [this, taskId = task.id](ExecutionOutcome<Result> outcome) {
                onTaskFinished(taskId, std::move(outcome));
            });
        }, &error);
        if (!dispatched) {
            onTaskFinished(task.id, ExecutionOutcome<Result>::failure({ TaskState::Failed, error, false }));
        }
    }

    void onTaskFinished(TaskId taskId, ExecutionOutcome<Result> outcome)
    {
        std::optional<PendingTask> completed;
        std::optional<PendingTask> next;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_active.has_value() || m_active->id != taskId) {
                return;
            }

            completed = m_active;
            m_active.reset();
            if (!m_waiting.empty()) {
                next = m_waiting.front();
                m_waiting.pop_front();
                m_active = next;
            }
        }

        if (completed.has_value() && completed->completion) {
            completed->completion(taskId, std::move(outcome));
        }
        if (next.has_value()) {
            startTask(*next);
        }
    }

    RuntimeHost *m_host = nullptr;
    LaneConfig m_config;
    Starter m_starter;
    std::shared_ptr<IWaitingMerger<Args>> m_merger;
    mutable QMutex m_mutex;
    TaskId m_nextTaskId = 0;
    std::optional<PendingTask> m_active;
    std::deque<PendingTask> m_waiting;
    bool m_shuttingDown = false;
};
