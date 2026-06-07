#pragma once

#include "combinators.h"
#include "exceptions.h"
#include "future.h"
#include "promise.h"
#include "timer_executor.h"
#include "try.h"
#include "unit.h"

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
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
    TimerTaskHandle timer;
    bool completed = false;
};

template <typename T>
bool tryCompleteFromFuture(const std::shared_ptr<TimeoutContext<T>> &context,
                           TimerTaskHandle *timerToCancel)
{
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->completed) {
        return false;
    }
    context->completed = true;
    if (timerToCancel) {
        *timerToCancel = std::move(context->timer);
    }
    return true;
}

template <typename T>
bool tryCompleteFromTimer(const std::shared_ptr<TimeoutContext<T>> &context)
{
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->completed) {
        return false;
    }
    context->completed = true;
    return true;
}

template <typename T>
bool storeTimerOrShouldCancel(const std::shared_ptr<TimeoutContext<T>> &context,
                              TimerTaskHandle &timer)
{
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->completed) {
        return true;
    }

    context->timer = std::move(timer);
    return false;
}
}

template <typename Rep, typename Period>
Future<Unit> sleepFor(std::chrono::duration<Rep, Period> duration)
{
    Promise<Unit> promise;
    Future<Unit> future = promise.getFuture();
    auto promiseHolder = std::make_shared<Promise<Unit>>(std::move(promise));

    TimerTaskHandle timer = TimerExecutor::instance()->scheduleAfter(duration, [promiseHolder]() mutable {
        promiseHolder->setValue();
    });
    if (!timer.scheduled()) {
        promiseHolder->setException(std::make_exception_ptr(ExecutorRejected()));
    }

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
    InterruptHandle interruptHandle = future.interruptHandle();

    std::move(future).thenTry([context](Try<T> &&result) {
        TimerTaskHandle timerToCancel;
        if (detail::tryCompleteFromFuture(context, &timerToCancel)) {
            timerToCancel.cancel();
            context->promise.setTry(std::move(result));
        }
        return Unit();
    });

    TimerTaskHandle timer = TimerExecutor::instance()->scheduleAfter(
        timeout,
        [context, interruptHandle]() mutable {
            std::exception_ptr timeoutException = std::make_exception_ptr(FutureTimeout());
            if (detail::tryCompleteFromTimer(context)) {
                context->promise.setException(timeoutException);
                interruptHandle.raise(timeoutException);
            }
        });

    if (!timer.scheduled()) {
        if (detail::tryCompleteFromTimer(context)) {
            context->promise.setException(std::make_exception_ptr(ExecutorRejected()));
        }
    } else if (detail::storeTimerOrShouldCancel(context, timer)) {
        timer.cancel();
    }

    return output;
}

template <typename T, typename Clock, typename Duration>
Future<T> within(Future<T> future, std::chrono::time_point<Clock, Duration> deadline)
{
    return within(std::move(future), deadline - Clock::now());
}

template <typename T, typename Rep, typename Period>
Future<T> delayed(Future<T> future, std::chrono::duration<Rep, Period> delay)
{
    if (!future.valid()) {
        return makeExceptionFuture<T>(std::make_exception_ptr(FutureInvalid()));
    }

    return std::move(future).thenTry([delay](Try<T> &&result) {
        auto resultHolder = std::make_shared<Try<T>>(std::move(result));
        return sleepFor(delay).thenValue([resultHolder]() mutable {
            return std::move(*resultHolder).value();
        });
    });
}

template <typename T, typename F, typename Rep, typename Period>
Future<T> onTimeout(Future<T> future, std::chrono::duration<Rep, Period> timeout, F &&func)
{
    using Handler = typename std::decay<F>::type;
    auto funcHolder = std::make_shared<Handler>(std::forward<F>(func));

    return within(std::move(future), timeout)
        .template thenError<FutureTimeout>(
            [funcHolder](const FutureTimeout &timeoutException) mutable {
                if constexpr (std::is_invocable<Handler &, const FutureTimeout &>::value) {
                    return (*funcHolder)(timeoutException);
                } else {
                    return (*funcHolder)();
                }
            });
}
}
