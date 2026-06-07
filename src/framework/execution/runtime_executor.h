#pragma once

#include "framework/execution/future/async_future.h"
#include "framework/execution/lane/lane_exceptions.h"
#include "framework/platform/runtime.h"

#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace execution
{
class RuntimeUnavailable final : public async::FutureException
{
public:
    RuntimeUnavailable()
        : async::FutureException("Runtime unavailable")
    {
    }

    explicit RuntimeUnavailable(const std::string &message)
        : async::FutureException(message)
    {
    }
};

class RuntimeInitializeFailed final : public async::FutureException
{
public:
    RuntimeInitializeFailed()
        : async::FutureException("Runtime initialize failed")
    {
    }

    explicit RuntimeInitializeFailed(const std::string &message)
        : async::FutureException(message)
    {
    }
};

class RuntimeExecutor final
{
public:
    RuntimeExecutor(std::unique_ptr<IRuntime> runtime,
                    std::shared_ptr<async::SingleThreadExecutor> executor = {});
    ~RuntimeExecutor();

    RuntimeExecutor(const RuntimeExecutor &) = delete;
    RuntimeExecutor &operator=(const RuntimeExecutor &) = delete;

    async::Future<async::Unit> initialize();

    template <typename F>
    auto submit(F &&func)
        -> async::Future<typename async::detail::FutureValue<std::invoke_result_t<F, IRuntime &>>::Type>
    {
        using RawResult = std::invoke_result_t<F, IRuntime &>;
        using Result = typename async::detail::FutureValue<RawResult>::Type;

        const auto state = m_state;
        if (!state || state->shutdown.load()) {
            return async::makeExceptionFuture<Result>(std::make_exception_ptr(ExecutorShutdown()));
        }
        if (!state->runtime || !state->executor || state->executor->isShutdown()) {
            return async::makeExceptionFuture<Result>(std::make_exception_ptr(RuntimeUnavailable()));
        }

        auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));
        return async::makeReadyFuture()
            .via(state->executor)
            .thenValue([state, funcHolder]() -> RawResult {
                if (state->shutdown.load() || !state->runtime) {
                    throw ExecutorShutdown();
                }
                return (*funcHolder)(*state->runtime);
            });
    }

    template <typename F>
    auto submitBlocking(F &&func)
        -> typename async::detail::FutureValue<std::invoke_result_t<F, IRuntime &>>::Type
    {
        using RawResult = std::invoke_result_t<F, IRuntime &>;
        using Result = typename async::detail::FutureValue<RawResult>::Type;

        const auto state = m_state;
        if (!state || state->shutdown.load()) {
            throw ExecutorShutdown();
        }
        if (!state->runtime || !state->executor || state->executor->isShutdown()) {
            throw RuntimeUnavailable();
        }

        if (state->executor->isOnExecutorThread()) {
            if (state->shutdown.load() || !state->runtime) {
                throw ExecutorShutdown();
            }
            if constexpr (std::is_void<RawResult>::value) {
                std::forward<F>(func)(*state->runtime);
                return async::Unit();
            } else {
                return std::forward<F>(func)(*state->runtime);
            }
        }

        return submit(std::forward<F>(func)).get();
    }

    async::Future<async::Unit> shutdown();
    bool isShutdown() const;
    bool isOnRuntimeThread() const;
    std::shared_ptr<async::SingleThreadExecutor> executor() const;

private:
    struct State final
    {
        State(std::unique_ptr<IRuntime> runtimeIn,
              std::shared_ptr<async::SingleThreadExecutor> executorIn);

        std::unique_ptr<IRuntime> runtime;
        std::shared_ptr<async::SingleThreadExecutor> executor;
        std::atomic<bool> initialized { false };
        std::atomic<bool> shutdown { false };
    };

    static std::string errorMessage(const QString &message, const char *fallback);

    std::shared_ptr<State> m_state;
};
}
