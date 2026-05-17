#pragma once

#include "framework/platform/runtime.h"

#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>
#include <utility>

using TaskId = quint64;

enum class QueuePolicyKind
{
    Immediate,
    Serial,
    MergeWhileBusy
};

enum class DeliveryPolicyKind
{
    DeliverEveryStartedResult,
    DropStaleStartedResults
};

enum class TaskState
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Rejected,
    Shutdown
};

struct ExecutionError final
{
    TaskState state = TaskState::Failed;
    QString message;
    bool retryable = false;
};

template <typename T>
class ExecutionOutcome final
{
public:
    static ExecutionOutcome success(T value)
    {
        return ExecutionOutcome(std::move(value));
    }

    static ExecutionOutcome failure(ExecutionError error)
    {
        return ExecutionOutcome(std::move(error));
    }

    bool ok() const
    {
        return m_ok;
    }

    const T &value() const
    {
        return *m_value;
    }

    T &value()
    {
        return *m_value;
    }

    const ExecutionError &error() const
    {
        return m_error;
    }

private:
    explicit ExecutionOutcome(T value)
        : m_ok(true)
        , m_value(std::move(value))
    {
    }

    explicit ExecutionOutcome(ExecutionError error)
        : m_ok(false)
        , m_error(std::move(error))
    {
    }

    bool m_ok = false;
    std::optional<T> m_value;
    ExecutionError m_error;
};

template <>
class ExecutionOutcome<void> final
{
public:
    static ExecutionOutcome success()
    {
        return ExecutionOutcome(true, {});
    }

    static ExecutionOutcome failure(ExecutionError error)
    {
        return ExecutionOutcome(false, std::move(error));
    }

    bool ok() const
    {
        return m_ok;
    }

    const ExecutionError &error() const
    {
        return m_error;
    }

private:
    ExecutionOutcome(bool ok, ExecutionError error)
        : m_ok(ok)
        , m_error(std::move(error))
    {
    }

    bool m_ok = false;
    ExecutionError m_error;
};

struct LaneConfig final
{
    QString laneId;
    QueuePolicyKind queuePolicy = QueuePolicyKind::Serial;
    DeliveryPolicyKind deliveryPolicy = DeliveryPolicyKind::DeliverEveryStartedResult;
    int maxWaitingCount = 1;
};

struct TaskContext final
{
    TaskId taskId = 0;
    IRuntime *runtime = nullptr;
};

struct SubmitResult final
{
    bool accepted = false;
    TaskId taskId = 0;
    ExecutionError error;
};

template <typename Args>
class IWaitingMerger
{
public:
    virtual ~IWaitingMerger() = default;

    virtual bool canMerge(const Args &waiting, const Args &incoming) const = 0;
    virtual Args merge(const Args &waiting, const Args &incoming) const = 0;
};

template <typename Args>
class ReplaceWaitingWithIncomingMerger final : public IWaitingMerger<Args>
{
public:
    bool canMerge(const Args &, const Args &) const override
    {
        return true;
    }

    Args merge(const Args &, const Args &incoming) const override
    {
        return incoming;
    }
};
