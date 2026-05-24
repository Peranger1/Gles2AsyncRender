#include "runtime_executor.h"

#include <QMutexLocker>

#include <utility>

namespace
{
ExecutionError makeError(TaskState state, const QString &message)
{
    ExecutionError error;
    error.state = state;
    error.message = message;
    return error;
}

SubmitResult rejected(TaskState state, const QString &message)
{
    SubmitResult result;
    result.error = makeError(state, message);
    return result;
}

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}
}

RuntimeExecutor::RuntimeExecutor(RuntimeHost *host)
    : m_host(host)
{
}

SubmitResult RuntimeExecutor::post(RuntimeTask task)
{
    RuntimeHost *host = nullptr;
    TaskId taskId = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shuttingDown) {
            return rejected(TaskState::Shutdown, QStringLiteral("The runtime executor has been shut down."));
        }
        if (!m_host) {
            return rejected(TaskState::Rejected, QStringLiteral("The runtime executor requires a valid runtime host."));
        }
        if (!task) {
            return rejected(TaskState::Rejected, QStringLiteral("The runtime executor cannot post a null task."));
        }

        host = m_host;
        taskId = ++m_nextTaskId;
    }

    SubmitResult result;
    result.taskId = taskId;

    QString errorText;
    const bool dispatched = host->dispatchAsync(std::move(task), &errorText);
    if (!dispatched) {
        result.error = makeError(TaskState::Failed,
                                 errorText.isEmpty()
                                     ? QStringLiteral("The runtime executor failed to dispatch an async task.")
                                     : errorText);
        return result;
    }

    result.accepted = true;
    return result;
}

bool RuntimeExecutor::call(RuntimeTask task, QString *error)
{
    RuntimeHost *host = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shuttingDown) {
            setError(error, QStringLiteral("The runtime executor has been shut down."));
            return false;
        }
        if (!m_host) {
            setError(error, QStringLiteral("The runtime executor requires a valid runtime host."));
            return false;
        }
        if (!task) {
            setError(error, QStringLiteral("The runtime executor cannot call a null task."));
            return false;
        }

        host = m_host;
    }

    QString errorText;
    const bool dispatched = host->dispatchSync(std::move(task), &errorText);
    if (!dispatched) {
        setError(error,
                 errorText.isEmpty()
                     ? QStringLiteral("The runtime executor failed to dispatch a sync task.")
                     : errorText);
        return false;
    }
    return true;
}

bool RuntimeExecutor::isOnRuntimeThread() const
{
    RuntimeHost *host = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        host = m_host;
    }
    return host && host->isOnRuntimeThread();
}

void RuntimeExecutor::shutdown()
{
    QMutexLocker locker(&m_mutex);
    m_shuttingDown = true;
}
