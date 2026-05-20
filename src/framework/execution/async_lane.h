#pragma once

#include "execution_common.h"
#include "runtime_host.h"

#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <deque>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace execution
{
struct ImmediateQueue final
{
    static constexpr QueuePolicyKind kind = QueuePolicyKind::Immediate;
    static constexpr int maxWaitingCount = 0;
};

template <int MaxWaitingCount = 1>
struct SerialQueue final
{
    static_assert(MaxWaitingCount >= 1, "SerialQueue must keep at least one waiting task.");
    static constexpr QueuePolicyKind kind = QueuePolicyKind::Serial;
    static constexpr int maxWaitingCount = MaxWaitingCount;
};

template <int MaxWaitingCount = 1>
struct MergeWhileBusyQueue final
{
    static_assert(MaxWaitingCount >= 1, "MergeWhileBusyQueue must keep at least one waiting task.");
    static constexpr QueuePolicyKind kind = QueuePolicyKind::MergeWhileBusy;
    static constexpr int maxWaitingCount = MaxWaitingCount;
};

struct DeliverEveryStartedResult final
{
    static constexpr DeliveryPolicyKind kind = DeliveryPolicyKind::DeliverEveryStartedResult;
};

struct DropStaleStartedResults final
{
    static constexpr DeliveryPolicyKind kind = DeliveryPolicyKind::DropStaleStartedResults;
};

struct RejectMerge final
{
    template <typename Args>
    bool canMerge(const Args &, const Args &) const
    {
        return false;
    }

    template <typename Args>
    Args merge(const Args &, Args incoming) const
    {
        return incoming;
    }
};

struct ReplaceWaitingWithIncoming final
{
    template <typename Args>
    bool canMerge(const Args &, const Args &) const
    {
        return true;
    }

    template <typename Args>
    Args merge(const Args &, Args incoming) const
    {
        return incoming;
    }
};
}

template <typename Args,
          typename Result,
          typename QueuePolicy = execution::SerialQueue<1>,
          typename DeliveryPolicy = execution::DeliverEveryStartedResult,
          typename WaitingMerger = execution::RejectMerge>
class AsyncLane
{
public:
    using Completion = std::function<void(TaskId, ExecutionOutcome<Result>)>;
    using Done = std::function<void(ExecutionOutcome<Result>)>;
    using Starter = std::function<void(const TaskContext &, const Args &, Done)>;

    AsyncLane(RuntimeHost *host, Starter starter, WaitingMerger merger = {})
        : m_host(host)
        , m_starter(std::move(starter))
        , m_merger(std::move(merger))
    {
    }

    SubmitResult submit(const Args &args, Completion completion)
    {
        return submit(Args(args), std::move(completion));
    }

    SubmitResult submit(Args &&args, Completion completion)
    {
        PendingTask task;
        task.id = ++m_nextTaskId;
        task.args = std::move(args);
        task.completion = std::move(completion);

        PendingTask activeToStart;
        std::optional<PendingTask> supersededToNotify;
        bool shouldStart = false;
        SubmitResult result;
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
                enqueueWhileBusy(std::move(task), &result, &supersededToNotify);
                if (!result.accepted) {
                    return result;
                }
            }
        }

        notifySuperseded(std::move(supersededToNotify));
        if (shouldStart) {
            startTask(activeToStart);
        }
        return result;
    }

    void shutdown()
    {
        std::deque<PendingTask> waiting;
        {
            QMutexLocker locker(&m_mutex);
            if (m_shuttingDown) {
                return;
            }
            m_shuttingDown = true;
            waiting = std::move(m_waiting);
            m_waiting.clear();
        }

        for (PendingTask &task : waiting) {
            failTask(task, TaskState::Shutdown, QStringLiteral("The async lane was shut down before the task completed."));
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

    void enqueueWhileBusy(PendingTask &&task,
                          SubmitResult *result,
                          std::optional<PendingTask> *supersededToNotify)
    {
        static_assert(QueuePolicy::kind == QueuePolicyKind::Immediate
                          || QueuePolicy::kind == QueuePolicyKind::Serial
                          || QueuePolicy::kind == QueuePolicyKind::MergeWhileBusy,
                      "Unsupported queue policy.");

        if constexpr (QueuePolicy::kind == QueuePolicyKind::Immediate) {
            result->error.state = TaskState::Rejected;
            result->error.message = QStringLiteral("The async lane rejected the task because it is busy.");
            return;
        } else if constexpr (QueuePolicy::kind == QueuePolicyKind::Serial) {
            if (m_waiting.size() >= std::size_t(QueuePolicy::maxWaitingCount)) {
                result->error.state = TaskState::Rejected;
                result->error.message = QStringLiteral("The async lane waiting queue is full.");
                return;
            }

            m_waiting.push_back(std::move(task));
            result->accepted = true;
        } else if constexpr (QueuePolicy::kind == QueuePolicyKind::MergeWhileBusy) {
            if (m_waiting.size() < std::size_t(QueuePolicy::maxWaitingCount)) {
                m_waiting.push_back(std::move(task));
                result->accepted = true;
                return;
            }

            if (m_waiting.empty() || !m_merger.canMerge(m_waiting.back().args, task.args)) {
                result->error.state = TaskState::Rejected;
                result->error.message = QStringLiteral("The async lane could not merge the waiting tail task.");
                return;
            }

            PendingTask supersededTask = std::move(m_waiting.back());
            PendingTask mergedTask = std::move(task);
            mergedTask.args = m_merger.merge(supersededTask.args, std::move(mergedTask.args));
            m_waiting.back() = std::move(mergedTask);
            *supersededToNotify = std::move(supersededTask);
            result->accepted = true;
        }
    }

    void notifySuperseded(std::optional<PendingTask> supersededToNotify)
    {
        if (!supersededToNotify.has_value()) {
            return;
        }
        failTask(*supersededToNotify,
                 TaskState::Superseded,
                 QStringLiteral("The waiting task was merged into a newer task."));
    }

    static void failTask(PendingTask &task, TaskState state, const QString &message)
    {
        if (!task.completion) {
            return;
        }
        ExecutionError error;
        error.state = state;
        error.message = message;
        task.completion(task.id, ExecutionOutcome<Result>::failure(std::move(error)));
    }

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
        bool shouldDeliverCompleted = true;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_active.has_value() || m_active->id != taskId) {
                return;
            }

            completed = m_active;
            if constexpr (DeliveryPolicy::kind == DeliveryPolicyKind::DropStaleStartedResults) {
                if (!m_waiting.empty()) {
                    shouldDeliverCompleted = false;
                }
            }
            m_active.reset();
            if (!m_shuttingDown && !m_waiting.empty()) {
                next = m_waiting.front();
                m_waiting.pop_front();
                m_active = next;
            }
        }

        if (shouldDeliverCompleted && completed.has_value() && completed->completion) {
            completed->completion(taskId, std::move(outcome));
        }
        if (next.has_value()) {
            startTask(*next);
        }
    }

    RuntimeHost *m_host = nullptr;
    Starter m_starter;
    WaitingMerger m_merger;
    mutable QMutex m_mutex;
    TaskId m_nextTaskId = 0;
    std::optional<PendingTask> m_active;
    std::deque<PendingTask> m_waiting;
    bool m_shuttingDown = false;
};
