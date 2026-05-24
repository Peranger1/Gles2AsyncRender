#pragma once

#include "execution_common.h"
#include "runtime_host.h"

#include <QMutex>
#include <QString>

#include <functional>

class RuntimeExecutor final
{
public:
    using RuntimeTask = std::function<void(IRuntime *)>;

    explicit RuntimeExecutor(RuntimeHost *host);

    SubmitResult post(RuntimeTask task);
    bool call(RuntimeTask task, QString *error);
    bool isOnRuntimeThread() const;

    // Rejects future submissions only. Tasks already dispatched into the host
    // event loop may still run and must be guarded by their owning layer.
    void shutdown();

private:
    RuntimeHost *m_host = nullptr;
    mutable QMutex m_mutex;
    TaskId m_nextTaskId = 0;
    bool m_shuttingDown = false;
};
