#pragma once

#include "executor.h"
#include "future.h"
#include "promise.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace async
{
// 将 callable 投递到 executor，并把普通值、void、Future<T> 统一提升为 Future<T>。
template <typename F>
auto async(std::shared_ptr<Executor> executor, F &&func)
    -> Future<typename detail::FutureValue<std::invoke_result_t<F>>::Type>
{
    using RawResult = std::invoke_result_t<F>;
    using Result = typename detail::FutureValue<RawResult>::Type;

    Promise<Result> promise;
    Future<Result> future = promise.getFuture();
    auto promiseHolder = std::make_shared<Promise<Result>>(std::move(promise));
    auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));

    const bool scheduled = detail::schedule(executor, [promiseHolder, funcHolder]() mutable {
        promiseHolder->setWith([&]() -> RawResult {
            return (*funcHolder)();
        });
    });
    if (!scheduled) {
        // 初始投递失败也要立即兑现 future，避免 caller 阻塞在 get()。
        promiseHolder->setException(std::make_exception_ptr(ExecutorRejected()));
    }

    return future;
}
}
