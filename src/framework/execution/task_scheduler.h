#pragma once

#include "framework/execution/execution_common.h"
#include "framework/execution/future/async_future.h"

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

        // 用 ready future 作为统一入口，借助 thenValue 支持普通值、void 和 Future<T> flatten。
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
    // executor 只负责实际调度；shutdown 是 scheduler 自己的提交闸门。
    std::shared_ptr<async::Executor> m_executor;
    std::atomic<bool> m_shutdown { false };
};
}
