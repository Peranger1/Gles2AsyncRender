#include "future_test_cases.h"

#include "framework/execution/future/async_future.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using execution_test::require;
using execution_test::requireThrows;

void sleepForCompletesAfterDuration()
{
    auto future = async::sleepFor(std::chrono::milliseconds(1));
    async::Unit unit = future.get();
    (void)unit;
}

void sleepUntilCompletesAtDeadline()
{
    auto future = async::sleepUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
    async::Unit unit = future.get();
    (void)unit;
}

void timerTaskHandleCancelsScheduledTask()
{
    async::TimerExecutor timer;
    std::atomic<bool> ran(false);

    auto handle = timer.scheduleAfter(std::chrono::milliseconds(30), [&]() {
        ran = true;
    });

    require(handle.scheduled(), "TimerTaskHandle should report scheduled task");
    require(handle.valid(), "TimerTaskHandle should be valid before cancellation");

    handle.cancel();
    require(!handle.valid(), "TimerTaskHandle should be invalid after cancellation");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!ran.load(), "cancelled timer task should not run");
}

void delayedDefersReadyValue()
{
    auto future = async::delayed(async::makeReadyFuture(42), std::chrono::milliseconds(20));

    require(!future.waitFor(std::chrono::milliseconds(1)),
            "delayed should not complete before the delay elapses");
    require(future.get() == 42, "delayed should preserve the original value");
}

void delayedDefersOriginalException()
{
    auto future = async::delayed(
        async::makeExceptionFuture<int>(std::make_exception_ptr(std::runtime_error("delayed"))),
        std::chrono::milliseconds(1));

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "delayed should preserve the original exception");
}

void delayedRejectsInvalidFuture()
{
    auto future = async::delayed(async::Future<int>(), std::chrono::milliseconds(1));

    require(future.isReady(), "delayed should reject an invalid future immediately");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "delayed should complete with FutureInvalid for invalid input");
}

void withinReturnsOriginalValueBeforeTimeout()
{
    auto future = async::within(async::makeReadyFuture(42), std::chrono::milliseconds(50));
    require(future.get() == 42, "within should return the original value when it completes first");
}

void withinPropagatesOriginalExceptionBeforeTimeout()
{
    auto future = async::within(
        async::makeExceptionFuture<int>(std::make_exception_ptr(std::runtime_error("original"))),
        std::chrono::milliseconds(50));

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "within should propagate the original exception when it completes first");
}

void withinTimesOutWhenDeadlineWins()
{
    async::Promise<int> promise;
    auto future = async::within(promise.getFuture(), std::chrono::milliseconds(1));

    requireThrows<async::FutureTimeout>([&]() {
        future.get();
    }, "within should fail with FutureTimeout when the timeout wins");
}

void withinIgnoresLateOriginalCompletion()
{
    async::Promise<int> promise;
    auto future = async::within(promise.getFuture(), std::chrono::milliseconds(1));

    requireThrows<async::FutureTimeout>([&]() {
        future.get();
    }, "within should time out before the original future completes");

    promise.setValue(42);
}

void withinTimeoutInterruptsOriginalPromise()
{
    async::Promise<int> promise;
    std::mutex mutex;
    std::condition_variable cv;
    bool interrupted = false;
    bool receivedTimeout = false;

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
            try {
                std::rethrow_exception(reason);
            } catch (const async::FutureTimeout &) {
                receivedTimeout = true;
            }
        }
        cv.notify_one();
    });

    auto future = async::within(promise.getFuture(), std::chrono::milliseconds(1));

    requireThrows<async::FutureTimeout>([&]() {
        future.get();
    }, "within should fail with FutureTimeout when the timeout wins");

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
            return interrupted;
        });
    }

    require(interrupted, "within timeout should interrupt the original promise");
    require(receivedTimeout, "within timeout should deliver FutureTimeout as interrupt reason");
}

void withinRejectsInvalidFuture()
{
    auto future = async::within(async::Future<int>(), std::chrono::milliseconds(1));

    require(future.isReady(), "within should reject an invalid future immediately");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "within should complete with FutureInvalid for invalid input");
}

void onTimeoutKeepsOriginalValueBeforeDeadline()
{
    auto future = async::onTimeout(
        async::makeReadyFuture(42),
        std::chrono::milliseconds(50),
        []() {
            return 7;
        });

    require(future.get() == 42, "onTimeout should preserve the original value before timeout");
}

void onTimeoutRecoversWithFallbackValue()
{
    async::Promise<int> promise;
    auto future = async::onTimeout(
        promise.getFuture(),
        std::chrono::milliseconds(1),
        []() {
            return 42;
        });

    require(future.get() == 42, "onTimeout should recover with a fallback value");
}

void onTimeoutRecoversWithFallbackFuture()
{
    async::Promise<int> promise;
    auto future = async::onTimeout(
        promise.getFuture(),
        std::chrono::milliseconds(1),
        []() {
            return async::makeReadyFuture(42);
        });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "onTimeout should flatten a fallback Future<T>");
    require(future.get() == 42, "onTimeout should recover with a fallback future");
}

void onTimeoutLeavesNonTimeoutException()
{
    auto future = async::onTimeout(
        async::makeExceptionFuture<int>(std::make_exception_ptr(std::runtime_error("original"))),
        std::chrono::milliseconds(50),
        []() {
            return 42;
        });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "onTimeout should only recover FutureTimeout");
}

void onTimeoutInterruptsOriginalPromise()
{
    async::Promise<int> promise;
    std::mutex mutex;
    std::condition_variable cv;
    bool interrupted = false;

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            try {
                std::rethrow_exception(reason);
            } catch (const async::FutureTimeout &) {
                interrupted = true;
            }
        }
        cv.notify_one();
    });

    auto future = async::onTimeout(
        promise.getFuture(),
        std::chrono::milliseconds(1),
        []() {
            return 42;
        });

    require(future.get() == 42, "onTimeout should recover after timeout");

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
            return interrupted;
        });
    }

    require(interrupted, "onTimeout should interrupt the original promise when timeout wins");
}
}

namespace async_future_test
{
std::vector<execution_test::TestCase> timeoutTestCases()
{
    return {
        { "sleepForCompletesAfterDuration", sleepForCompletesAfterDuration },
        { "sleepUntilCompletesAtDeadline", sleepUntilCompletesAtDeadline },
        { "timerTaskHandleCancelsScheduledTask", timerTaskHandleCancelsScheduledTask },
        { "delayedDefersReadyValue", delayedDefersReadyValue },
        { "delayedDefersOriginalException", delayedDefersOriginalException },
        { "delayedRejectsInvalidFuture", delayedRejectsInvalidFuture },
        { "withinReturnsOriginalValueBeforeTimeout", withinReturnsOriginalValueBeforeTimeout },
        { "withinPropagatesOriginalExceptionBeforeTimeout", withinPropagatesOriginalExceptionBeforeTimeout },
        { "withinTimesOutWhenDeadlineWins", withinTimesOutWhenDeadlineWins },
        { "withinIgnoresLateOriginalCompletion", withinIgnoresLateOriginalCompletion },
        { "withinTimeoutInterruptsOriginalPromise", withinTimeoutInterruptsOriginalPromise },
        { "withinRejectsInvalidFuture", withinRejectsInvalidFuture },
        { "onTimeoutKeepsOriginalValueBeforeDeadline", onTimeoutKeepsOriginalValueBeforeDeadline },
        { "onTimeoutRecoversWithFallbackValue", onTimeoutRecoversWithFallbackValue },
        { "onTimeoutRecoversWithFallbackFuture", onTimeoutRecoversWithFallbackFuture },
        { "onTimeoutLeavesNonTimeoutException", onTimeoutLeavesNonTimeoutException },
        { "onTimeoutInterruptsOriginalPromise", onTimeoutInterruptsOriginalPromise },
    };
}
}
