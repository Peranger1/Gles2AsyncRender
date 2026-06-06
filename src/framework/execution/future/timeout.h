#pragma once

#include "combinators.h"
#include "exceptions.h"
#include "future.h"
#include "promise.h"
#include "try.h"
#include "unit.h"

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace async
{
namespace detail
{
template <typename T>
struct TimeoutContext final
{
    Promise<T> promise;
    std::mutex mutex;
    bool completed = false;
};

template <typename T>
bool tryMarkCompleted(const std::shared_ptr<TimeoutContext<T>> &context)
{
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->completed) {
        return false;
    }
    context->completed = true;
    return true;
}
}

template <typename Rep, typename Period>
Future<Unit> sleepFor(std::chrono::duration<Rep, Period> duration)
{
    Promise<Unit> promise;
    Future<Unit> future = promise.getFuture();
    auto promiseHolder = std::make_shared<Promise<Unit>>(std::move(promise));

    std::thread([promiseHolder, duration]() mutable {
        if (duration > duration.zero()) {
            std::this_thread::sleep_for(duration);
        }
        promiseHolder->setValue();
    }).detach();

    return future;
}

template <typename Clock, typename Duration>
Future<Unit> sleepUntil(std::chrono::time_point<Clock, Duration> timePoint)
{
    return sleepFor(timePoint - Clock::now());
}

template <typename T, typename Rep, typename Period>
Future<T> within(Future<T> future, std::chrono::duration<Rep, Period> timeout)
{
    if (!future.valid()) {
        return makeExceptionFuture<T>(std::make_exception_ptr(FutureInvalid()));
    }

    auto context = std::make_shared<detail::TimeoutContext<T>>();
    Future<T> output = context->promise.getFuture();

    std::move(future).thenTry([context](Try<T> &&result) {
        if (detail::tryMarkCompleted(context)) {
            context->promise.setTry(std::move(result));
        }
        return Unit();
    });

    std::thread([context, timeout]() mutable {
        if (timeout > timeout.zero()) {
            std::this_thread::sleep_for(timeout);
        }
        if (detail::tryMarkCompleted(context)) {
            context->promise.setException(std::make_exception_ptr(FutureTimeout()));
        }
    }).detach();

    return output;
}

template <typename T, typename Clock, typename Duration>
Future<T> within(Future<T> future, std::chrono::time_point<Clock, Duration> deadline)
{
    return within(std::move(future), deadline - Clock::now());
}
}
