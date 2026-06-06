#pragma once

#include "framework/execution/future/async_future.h"
#include "framework/execution/lane/lane_exceptions.h"

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace execution
{
class TaskScheduler final
{
public:
    explicit TaskScheduler(std::shared_ptr<async::Executor> executor)
        : m_executor(std::move(executor))
    {
    }

    template <typename F>
    auto submit(F &&func)
        -> async::Future<typename async::detail::FutureValue<std::invoke_result_t<F>>::Type>
    {
        using RawResult = std::invoke_result_t<F>;
        using Result = typename async::detail::FutureValue<RawResult>::Type;

        if (m_shutdown.load()) {
            return async::makeExceptionFuture<Result>(std::make_exception_ptr(ExecutorShutdown()));
        }
        if (!m_executor) {
            return async::makeExceptionFuture<Result>(std::make_exception_ptr(TaskRejected()));
        }

        return async::makeReadyFuture()
            .via(m_executor)
            .thenValue(std::forward<F>(func));
    }

    void shutdown()
    {
        m_shutdown.store(true);
    }

    bool isShutdown() const
    {
        return m_shutdown.load();
    }

private:
    std::shared_ptr<async::Executor> m_executor;
    std::atomic<bool> m_shutdown { false };
};
}
