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

void thenValueTransformsSuccessfulResult()
{
    async::Promise<int> promise;
    auto future = promise.getFuture()
        .thenValue([](int value) {
            return value + 1;
        })
        .thenValue([](int value) {
            return value * 2;
        });

    promise.setValue(20);
    require(future.get() == 42, "thenValue should transform successful results");
}

void thenValuePropagatesExceptionToThenError()
{
    async::Promise<int> promise;
    auto future = promise.getFuture()
        .thenValue([](int value) {
            return value + 1;
        })
        .thenError([](std::exception_ptr exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::runtime_error &e) {
                require(std::string(e.what()) == "boom", "thenError should receive the original exception");
            }
            return 7;
        });

    promise.setException(std::make_exception_ptr(std::runtime_error("boom")));
    require(future.get() == 7, "thenError should recover from exceptions");
}

void typedThenErrorRecoversMatchingException()
{
    auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("typed")))
        .thenError<std::runtime_error>([](const std::runtime_error &exception) {
            require(std::string(exception.what()) == "typed",
                    "typed thenError should receive the matched exception");
            return 42;
        });

    require(future.get() == 42, "typed thenError should recover matching exceptions");
}

void typedThenErrorLeavesMismatchedException()
{
    auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("typed")))
        .thenError<std::logic_error>([](const std::logic_error &) {
            return 42;
        });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "typed thenError should propagate mismatched exceptions");
}

void typedThenErrorFlattensReturnedFuture()
{
    auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("typed")))
        .thenError<std::runtime_error>([](const std::runtime_error &) {
            return async::makeReadyFuture(42);
        });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "typed thenError should flatten returned Future<T>");
    require(future.get() == 42, "typed thenError should flatten returned Future<T>");
}

void thenTryObservesValueAndException()
{
    async::Promise<int> valuePromise;
    auto valueFuture = valuePromise.getFuture().thenTry([](async::Try<int> &&result) {
        require(result.hasValue(), "thenTry should observe successful values");
        return std::move(result).value() + 1;
    });
    valuePromise.setValue(10);
    require(valueFuture.get() == 11, "thenTry should transform values");

    async::Promise<int> errorPromise;
    auto errorFuture = errorPromise.getFuture().thenTry([](async::Try<int> &&result) {
        require(result.hasException(), "thenTry should observe exceptions");
        return 5;
    });
    errorPromise.setException(std::make_exception_ptr(std::runtime_error("bad")));
    require(errorFuture.get() == 5, "thenTry should transform exceptions");
}

void readyFutureRunsContinuationWhenAttached()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    promise.setValue(41);

    auto next = future.thenValue([](int value) {
        return value + 1;
    });

    require(next.isReady(), "continuation attached to a ready future should run immediately inline");
    require(next.get() == 42, "ready future continuation should receive the stored result");
}

void thenValueFlattensReturnedFuture()
{
    auto executor = async::InlineExecutor::instance();
    auto future = async::async(executor, []() {
        return 10;
    }).thenValue([executor](int value) {
        return async::async(executor, [value]() {
            return value + 32;
        });
    });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "thenValue should flatten returned Future<T>");
    require(future.get() == 42, "thenValue should flatten returned Future<T>");
}

void thenTryFlattensReturnedFuture()
{
    auto executor = async::InlineExecutor::instance();
    async::Promise<int> promise;
    auto future = promise.getFuture().thenTry([executor](async::Try<int> &&result) {
        require(result.hasValue(), "thenTry should receive the original value before flattening");
        const int value = std::move(result).value();
        return async::async(executor, [value]() {
            return value * 2;
        });
    });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "thenTry should flatten returned Future<T>");
    promise.setValue(21);
    require(future.get() == 42, "thenTry should flatten returned Future<T>");
}

void thenErrorFlattensReturnedFuture()
{
    auto executor = async::InlineExecutor::instance();
    async::Promise<int> promise;
    auto future = promise.getFuture().thenError([executor](std::exception_ptr) {
        return async::async(executor, []() {
            return 42;
        });
    });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "thenError should flatten returned Future<T>");
    promise.setException(std::make_exception_ptr(std::runtime_error("recover")));
    require(future.get() == 42, "thenError should flatten returned Future<T>");
}

void ensureRunsAfterValueAndPreservesResult()
{
    int calls = 0;
    auto future = async::makeReadyFuture(41)
        .ensure([&]() {
            ++calls;
        })
        .thenValue([](int value) {
            return value + 1;
        });

    require(calls == 1, "ensure should run after a successful result");
    require(future.get() == 42, "ensure should preserve a successful result");
}

void ensureRunsAfterExceptionAndPreservesException()
{
    int calls = 0;
    auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("original")))
        .ensure([&]() {
            ++calls;
        });

    require(calls == 1, "ensure should run after an exceptional result");
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "ensure should preserve the original exception");
}

void ensureExceptionOverridesOriginalResult()
{
    auto future = async::makeReadyFuture(42)
        .ensure([]() {
            throw std::runtime_error("cleanup failed");
        });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "ensure should surface cleanup exceptions");
}

void ensureRunsThroughViaExecutor()
{
    auto executor = std::make_shared<async::ManualExecutor>();
    async::Promise<int> promise;
    bool cleanupRan = false;

    auto future = std::move(promise.getFuture())
        .via(executor)
        .ensure([&]() {
            cleanupRan = true;
        });

    promise.setValue(42);
    require(!cleanupRan, "ensure should be scheduled through the via executor");
    require(executor->queuedCount() == 1, "ensure continuation should be queued");

    executor->drain();
    require(cleanupRan, "ensure should run when the via executor drains");
    require(future.get() == 42, "ensure through via executor should preserve the value");
}

void flattenedFuturePropagatesInnerException()
{
    auto executor = async::InlineExecutor::instance();
    auto future = async::async(executor, []() {
        return 1;
    }).thenValue([executor](int) {
        return async::async(executor, []() -> int {
            throw std::runtime_error("inner");
        });
    }).thenError([](std::exception_ptr exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::runtime_error &e) {
            require(std::string(e.what()) == "inner",
                    "flattened Future should propagate the inner exception");
        }
        return 42;
    });

    require(future.get() == 42, "thenError should recover from a flattened inner exception");
}

void unitFutureSupportsVoidProducerAndContinuation()
{
    int touched = 0;
    auto future = async::async(async::InlineExecutor::instance(), [&]() {
        touched = 3;
    }).thenValue([&]() {
        touched += 4;
    });

    async::Unit unit = future.get();
    (void)unit;
    require(touched == 7, "Unit future should support void producers and continuations");
}
}

namespace async_future_test
{
std::vector<execution_test::TestCase> continuationTestCases()
{
    return {
        { "thenValueTransformsSuccessfulResult", thenValueTransformsSuccessfulResult },
        { "thenValuePropagatesExceptionToThenError", thenValuePropagatesExceptionToThenError },
        { "typedThenErrorRecoversMatchingException", typedThenErrorRecoversMatchingException },
        { "typedThenErrorLeavesMismatchedException", typedThenErrorLeavesMismatchedException },
        { "typedThenErrorFlattensReturnedFuture", typedThenErrorFlattensReturnedFuture },
        { "thenTryObservesValueAndException", thenTryObservesValueAndException },
        { "readyFutureRunsContinuationWhenAttached", readyFutureRunsContinuationWhenAttached },
        { "thenValueFlattensReturnedFuture", thenValueFlattensReturnedFuture },
        { "thenTryFlattensReturnedFuture", thenTryFlattensReturnedFuture },
        { "thenErrorFlattensReturnedFuture", thenErrorFlattensReturnedFuture },
        { "ensureRunsAfterValueAndPreservesResult", ensureRunsAfterValueAndPreservesResult },
        { "ensureRunsAfterExceptionAndPreservesException", ensureRunsAfterExceptionAndPreservesException },
        { "ensureExceptionOverridesOriginalResult", ensureExceptionOverridesOriginalResult },
        { "ensureRunsThroughViaExecutor", ensureRunsThroughViaExecutor },
        { "flattenedFuturePropagatesInnerException", flattenedFuturePropagatesInnerException },
        { "unitFutureSupportsVoidProducerAndContinuation", unitFutureSupportsVoidProducerAndContinuation },
    };
}
}
