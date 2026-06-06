#include "runtime_executor.h"

#include <utility>

namespace execution
{
RuntimeExecutor::State::State(std::unique_ptr<IRuntime> runtimeIn,
                              std::shared_ptr<async::SingleThreadExecutor> executorIn)
    : runtime(std::move(runtimeIn))
    , executor(std::move(executorIn))
{
    if (!executor) {
        executor = std::make_shared<async::SingleThreadExecutor>("runtime");
    }
}

RuntimeExecutor::RuntimeExecutor(std::unique_ptr<IRuntime> runtime,
                                 std::shared_ptr<async::SingleThreadExecutor> executor)
    : m_state(std::make_shared<State>(std::move(runtime), std::move(executor)))
{
}

RuntimeExecutor::~RuntimeExecutor()
{
    try {
        if (m_state && m_state->executor && !m_state->executor->isOnExecutorThread()) {
            shutdown().get();
        }
    } catch (...) {
    }
}

async::Future<async::Unit> RuntimeExecutor::initialize()
{
    const auto state = m_state;
    if (!state || state->shutdown.load()) {
        return async::makeExceptionFuture<async::Unit>(std::make_exception_ptr(ExecutorShutdown()));
    }
    if (!state->runtime || !state->executor || state->executor->isShutdown()) {
        return async::makeExceptionFuture<async::Unit>(std::make_exception_ptr(RuntimeUnavailable()));
    }

    return async::makeReadyFuture()
        .via(state->executor)
        .thenValue([state]() {
            if (state->shutdown.load() || !state->runtime) {
                throw ExecutorShutdown();
            }
            if (state->initialized.load()) {
                return;
            }

            QString error;
            if (!state->runtime->initialize(&error)) {
                throw RuntimeInitializeFailed(errorMessage(error, "Runtime initialize failed"));
            }
            state->initialized.store(true);
        });
}

async::Future<async::Unit> RuntimeExecutor::shutdown()
{
    const auto state = m_state;
    if (!state) {
        return async::makeReadyFuture();
    }
    if (state->shutdown.exchange(true)) {
        return async::makeReadyFuture();
    }
    if (!state->executor || state->executor->isShutdown()) {
        if (state->runtime) {
            state->runtime->shutdown();
            state->runtime.reset();
        }
        return async::makeReadyFuture();
    }

    return async::makeReadyFuture()
        .via(state->executor)
        .thenValue([state]() {
            if (state->runtime) {
                state->runtime->shutdown();
                state->runtime.reset();
            }
            state->initialized.store(false);
        });
}

bool RuntimeExecutor::isShutdown() const
{
    return !m_state || m_state->shutdown.load();
}

bool RuntimeExecutor::isOnRuntimeThread() const
{
    return m_state && m_state->executor && m_state->executor->isOnExecutorThread();
}

std::shared_ptr<async::SingleThreadExecutor> RuntimeExecutor::executor() const
{
    return m_state ? m_state->executor : nullptr;
}

std::string RuntimeExecutor::errorMessage(const QString &message, const char *fallback)
{
    return message.isEmpty() ? std::string(fallback) : message.toStdString();
}
}
