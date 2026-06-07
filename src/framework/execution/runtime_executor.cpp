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
        // 默认创建专属 runtime 线程，避免通用 execution 任务误入平台 runtime。
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
            // 析构里的 shutdown 是兜底路径；正常调用方仍应显式 shutdown 并观察结果。
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
                // initialize 幂等：已经初始化后直接完成，不重复触碰平台 runtime。
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
        // shutdown 幂等：只有第一次调用实际释放 runtime。
        return async::makeReadyFuture();
    }
    if (!state->executor || state->executor->isShutdown()) {
        if (state->runtime) {
            // executor 已不可用时只能在当前线程兜底释放 runtime。
            state->runtime->shutdown();
            state->runtime.reset();
        }
        return async::makeReadyFuture();
    }

    return async::makeReadyFuture()
        .via(state->executor)
        .thenValue([state]() {
            if (state->runtime) {
                // 正常路径把 runtime shutdown 和释放固定在 runtime 线程。
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
