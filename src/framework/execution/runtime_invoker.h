#pragma once

#include "execution_common.h"
#include "runtime_host.h"

#include <functional>

class RuntimeInvoker final
{
public:
    explicit RuntimeInvoker(RuntimeHost *host)
        : m_host(host)
    {
    }

    template <typename Result>
    using DirectFn = std::function<ExecutionOutcome<Result>()>;

    template <typename Result>
    using RuntimeFn = std::function<ExecutionOutcome<Result>(IRuntime *)>;

    template <typename Result>
    ExecutionOutcome<Result> callDirect(DirectFn<Result> fn) const
    {
        if (!fn) {
            ExecutionError error;
            error.message = QStringLiteral("callDirect requires a valid callable.");
            return ExecutionOutcome<Result>::failure(std::move(error));
        }
        return fn();
    }

    template <typename Result>
    ExecutionOutcome<Result> invokeSync(RuntimeFn<Result> fn) const
    {
        if (!m_host || !fn) {
            ExecutionError error;
            error.message = QStringLiteral("invokeSync requires a valid host and callable.");
            return ExecutionOutcome<Result>::failure(std::move(error));
        }

        ExecutionError initialError;
        ExecutionOutcome<Result> outcome = ExecutionOutcome<Result>::failure(std::move(initialError));
        QString errorText;
        const bool ok = m_host->dispatchSync([&](IRuntime *runtime) {
            outcome = fn(runtime);
        }, &errorText);
        if (!ok) {
            ExecutionError error;
            error.message = errorText;
            return ExecutionOutcome<Result>::failure(std::move(error));
        }
        return outcome;
    }

private:
    RuntimeHost *m_host = nullptr;
};
