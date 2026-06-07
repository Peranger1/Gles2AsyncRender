#pragma once

#include "future.h"
#include "promise.h"
#include "try.h"
#include "unit.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace async
{
template <typename T>
Future<T> makeReadyFuture(T value)
{
    Promise<T> promise;
    Future<T> future = promise.getFuture();
    promise.setValue(std::move(value));
    return future;
}

inline Future<Unit> makeReadyFuture()
{
    Promise<Unit> promise;
    Future<Unit> future = promise.getFuture();
    promise.setValue();
    return future;
}

template <typename T>
Future<T> makeExceptionFuture(std::exception_ptr exception)
{
    Promise<T> promise;
    Future<T> future = promise.getFuture();
    promise.setException(std::move(exception));
    return future;
}

template <typename T>
Future<T> makeExceptionFuture(const std::exception &exception)
{
    return makeExceptionFuture<T>(std::make_exception_ptr(exception));
}

namespace detail
{
template <typename T>
struct CollectAllContext final
{
    explicit CollectAllContext(std::size_t count)
        : remaining(count)
        , results(count)
    {
    }

    std::mutex mutex;
    // remaining/completed 共同防止最后一个结果之外的路径重复兑现 promise。
    std::size_t remaining = 0;
    std::vector<Try<T>> results;
    Promise<std::vector<Try<T>>> promise;
    bool completed = false;
};

template <typename T>
struct CollectAnyContext final
{
    Promise<std::pair<std::size_t, Try<T>>> promise;
    std::mutex mutex;
    bool completed = false;
};
}

template <typename T>
Future<std::vector<Try<T>>> collectAll(std::vector<Future<T>> futures)
{
    if (futures.empty()) {
        return makeReadyFuture(std::vector<Try<T>>());
    }

    for (const Future<T> &future : futures) {
        if (!future.valid()) {
            return makeExceptionFuture<std::vector<Try<T>>>(std::make_exception_ptr(FutureInvalid()));
        }
    }

    auto context = std::make_shared<detail::CollectAllContext<T>>(futures.size());
    Future<std::vector<Try<T>>> output = context->promise.getFuture();

    // collectAll 保留每个输入的 Try，不因单个异常提前失败。
    for (std::size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i]).thenTry([context, i](Try<T> &&result) {
            bool shouldComplete = false;
            std::vector<Try<T>> completedResults;

            {
                std::lock_guard<std::mutex> lock(context->mutex);
                context->results[i] = std::move(result);
                if (context->remaining > 0) {
                    --context->remaining;
                }

                if (context->remaining == 0 && !context->completed) {
                    context->completed = true;
                    completedResults = std::move(context->results);
                    shouldComplete = true;
                }
            }

            if (shouldComplete) {
                context->promise.setValue(std::move(completedResults));
            }
            return Unit();
        });
    }

    return output;
}

template <typename T>
Future<std::vector<T>> collect(std::vector<Future<T>> futures)
{
    return collectAll(std::move(futures)).thenValue([](std::vector<Try<T>> &&results) {
        std::vector<T> values;
        values.reserve(results.size());
        for (Try<T> &result : results) {
            values.push_back(std::move(result).value());
        }
        return values;
    });
}

template <typename T>
Future<std::pair<std::size_t, Try<T>>> collectAny(std::vector<Future<T>> futures)
{
    using Result = std::pair<std::size_t, Try<T>>;

    if (futures.empty()) {
        return makeExceptionFuture<Result>(std::make_exception_ptr(FutureInvalid()));
    }

    for (const Future<T> &future : futures) {
        if (!future.valid()) {
            return makeExceptionFuture<Result>(std::make_exception_ptr(FutureInvalid()));
        }
    }

    auto context = std::make_shared<detail::CollectAnyContext<T>>();
    Future<Result> output = context->promise.getFuture();

    // 第一个完成的输入胜出；成功和异常都会作为 Try 一并返回。
    for (std::size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i]).thenTry([context, i](Try<T> &&result) {
            std::optional<Result> completedResult;

            {
                std::lock_guard<std::mutex> lock(context->mutex);
                if (!context->completed) {
                    context->completed = true;
                    completedResult.emplace(i, std::move(result));
                }
            }

            if (completedResult.has_value()) {
                context->promise.setValue(std::move(*completedResult));
            }
            return Unit();
        });
    }

    return output;
}
}
