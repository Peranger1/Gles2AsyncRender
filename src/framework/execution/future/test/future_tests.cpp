#include "framework/execution/future/async_future.h"

#include <exception>
#include <functional>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
class TestFailure final : public std::runtime_error
{
public:
    explicit TestFailure(const std::string &message)
        : std::runtime_error(message)
    {
    }
};

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw TestFailure(message);
    }
}

template <typename Exception, typename F>
void requireThrows(F &&func, const std::string &message)
{
    try {
        func();
    } catch (const Exception &) {
        return;
    } catch (const std::exception &e) {
        throw TestFailure(message + ": threw unexpected exception `" + e.what() + "`");
    } catch (...) {
        throw TestFailure(message + ": threw unexpected non-standard exception");
    }

    throw TestFailure(message + ": did not throw");
}

void promiseCanBeFulfilledBeforeFutureRetrieval()
{
    async::Promise<int> promise;
    promise.setValue(42);

    auto future = promise.getFuture();
    require(future.isReady(), "future should be ready after promise was fulfilled");
    require(future.get() == 42, "future should return the fulfilled value");
}

void promiseRejectsDuplicateFutureRetrieval()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    (void)future;

    requireThrows<async::FutureAlreadyRetrieved>([&]() {
        promise.getFuture();
    }, "promise should reject duplicate future retrieval");
}

void promiseRejectsDuplicateFulfillment()
{
    async::Promise<int> promise;
    promise.setValue(1);

    requireThrows<async::PromiseAlreadySatisfied>([&]() {
        promise.setValue(2);
    }, "promise should reject duplicate fulfillment");
}

void futureCannotBeUsedAfterGet()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    promise.setValue(42);

    require(future.get() == 42, "future should return the first result");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "future should be invalid after get");
}

void futureCannotBeUsedAfterThenValue()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    auto next = future.thenValue([](int value) {
        return value + 1;
    });

    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "future should be invalid after attaching a continuation");

    promise.setValue(41);
    require(next.get() == 42, "downstream future should still receive the result");
}

void brokenPromiseCompletesFutureWithException()
{
    async::Future<int> future;
    {
        async::Promise<int> promise;
        future = promise.getFuture();
    }

    requireThrows<async::BrokenPromise>([&]() {
        future.get();
    }, "destroying an unfulfilled promise should complete with BrokenPromise");
}

void cancelAfterHandlerNotifiesProducer()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool interrupted = false;

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        interrupted = true;
        try {
            std::rethrow_exception(reason);
        } catch (const async::FutureCancelled &) {
            promise.setValue(42);
        }
    });

    future.cancel();
    require(interrupted, "cancel should notify the registered interrupt handler");
    require(future.get() == 42, "interrupt handler should be able to fulfill the promise");
}

void cancelBeforeHandlerIsDeliveredWhenHandlerIsSet()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool interrupted = false;

    future.cancel();
    require(!future.isReady(), "cancel should not complete the future by itself");

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        interrupted = true;
        try {
            std::rethrow_exception(reason);
        } catch (const async::FutureCancelled &) {
            promise.setException(std::make_exception_ptr(std::runtime_error("cancelled")));
        }
    });

    require(interrupted, "stored cancel should be delivered when handler is registered");
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "handler should be able to complete the future with its own exception");
}

void raiseDeliversCustomReason()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool receivedCustomReason = false;

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        try {
            std::rethrow_exception(reason);
        } catch (const std::runtime_error &e) {
            receivedCustomReason = std::string(e.what()) == "stop";
        }
        promise.setValue(42);
    });

    future.raise(std::make_exception_ptr(std::runtime_error("stop")));
    require(receivedCustomReason, "raise should deliver the custom interrupt reason");
    require(future.get() == 42, "future should still be completed by the producer");
}

void cancelAfterFulfilledDoesNothing()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool interrupted = false;

    promise.setInterruptHandler([&](std::exception_ptr) {
        interrupted = true;
    });
    promise.setValue(42);

    future.cancel();
    require(!interrupted, "cancel after fulfillment should not notify the handler");
    require(future.get() == 42, "cancel after fulfillment should not change the result");
}

void interruptHandlerRunsOutsideSharedStateLock()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();

    promise.setInterruptHandler([&](std::exception_ptr) {
        promise.setValue(42);
    });

    future.cancel();
    require(future.get() == 42, "interrupt handler should be able to complete without deadlock");
}

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

void nullExecutorFallsBackToInlineExecution()
{
    auto future = async::async(nullptr, []() {
        return 41;
    }).thenValue([](int value) {
        return value + 1;
    });

    require(future.isReady(), "null executor should fall back to inline execution");
    require(future.get() == 42, "null executor fallback should produce a value");
}

void continuationRunsThroughViaExecutor()
{
    auto executor = std::make_shared<async::ManualExecutor>();
    async::Promise<int> promise;

    auto future = std::move(promise.getFuture())
        .via(executor)
        .thenValue([](int value) {
            return value + 1;
        });

    promise.setValue(41);
    require(executor->queuedCount() == 1, "via executor should receive the continuation");
    require(!future.isReady(), "downstream future should wait until the executor drains");

    executor->drain();
    require(future.isReady(), "downstream future should become ready after executor drain");
    require(future.get() == 42, "via executor should run the continuation");
}

void asyncRunsOnThreadPoolExecutor()
{
    auto executor = std::make_shared<async::ThreadPoolExecutor>(2);
    auto future = async::async(executor, []() {
        return 40;
    }).thenValue([](int value) {
        return value + 2;
    });

    require(future.get() == 42, "async should run through a thread pool executor");
}

void asyncRunsOnThreadExecutor()
{
    auto executor = std::make_shared<async::ThreadExecutor>();
    auto future = async::async(executor, []() {
        return 42;
    });

    require(future.get() == 42, "async should run through a detached thread executor");
}

void manualExecutorQueuesUntilDrained()
{
    auto executor = std::make_shared<async::ManualExecutor>();
    int value = 0;

    executor->add([&]() {
        value = 42;
    });

    require(value == 0, "ManualExecutor should not run tasks until drained");
    require(executor->queuedCount() == 1, "ManualExecutor should report queued tasks");
    require(executor->drainOne(), "ManualExecutor should drain one task");
    require(value == 42, "ManualExecutor should run drained task");
    require(executor->empty(), "ManualExecutor should be empty after draining task");
}

void serialExecutorRunsQueuedTasksInOrderOnDrain()
{
    auto underlying = std::make_shared<async::ManualExecutor>();
    auto executor = std::make_shared<async::SerialExecutor>(underlying);
    std::vector<int> order;

    executor->add([&]() {
        order.push_back(1);
    });
    executor->add([&]() {
        order.push_back(2);
    });
    executor->add([&]() {
        order.push_back(3);
    });

    require(order.empty(), "SerialExecutor should wait for the underlying executor");
    require(underlying->queuedCount() == 1,
            "SerialExecutor should schedule a single drain task while active");

    underlying->drainOne();
    require(order.size() == 3, "SerialExecutor should drain queued tasks");
    require(order[0] == 1 && order[1] == 2 && order[2] == 3,
            "SerialExecutor should preserve FIFO order");
}

void serialExecutorDoesNotRunNestedAddsReentrantly()
{
    auto underlying = std::make_shared<async::ManualExecutor>();
    auto executor = std::make_shared<async::SerialExecutor>(underlying);
    std::vector<int> order;

    executor->add([&]() {
        order.push_back(1);
        executor->add([&]() {
            order.push_back(3);
        });
        order.push_back(2);
    });

    underlying->drain();
    require(order.size() == 3, "SerialExecutor should run nested queued task");
    require(order[0] == 1 && order[1] == 2 && order[2] == 3,
            "SerialExecutor should queue nested adds instead of running them inline");
}

void serialExecutorRejectsAfterShutdown()
{
    auto underlying = std::make_shared<async::ManualExecutor>();
    async::SerialExecutor executor(underlying);
    executor.shutdown();

    bool ran = false;
    executor.add([&]() {
        ran = true;
    });

    underlying->drain();
    require(!ran, "SerialExecutor should reject tasks after shutdown");
}

void singleThreadExecutorRunsTasksOnOneThreadInFifoOrder()
{
    auto executor = std::make_shared<async::SingleThreadExecutor>("test-single-thread");
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> order;
    std::vector<std::thread::id> threadIds;
    bool allTasksFinished = false;

    for (int i = 0; i < 3; ++i) {
        executor->add([&, i]() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(i);
                threadIds.push_back(std::this_thread::get_id());
                allTasksFinished = order.size() == 3;
            }
            cv.notify_one();
        });
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
            return allTasksFinished;
        });
    }

    require(order.size() == 3, "SingleThreadExecutor should run all queued tasks");
    require(order[0] == 0 && order[1] == 1 && order[2] == 2,
            "SingleThreadExecutor should run tasks in FIFO order");
    require(threadIds[0] == threadIds[1] && threadIds[1] == threadIds[2],
            "SingleThreadExecutor should run all tasks on the same thread");
    require(threadIds[0] != std::this_thread::get_id(),
            "SingleThreadExecutor should use its own worker thread");
}

void singleThreadExecutorReportsExecutorThread()
{
    auto executor = std::make_shared<async::SingleThreadExecutor>();
    auto future = async::async(executor, [executor]() {
        return executor->isOnExecutorThread();
    });

    require(!executor->isOnExecutorThread(),
            "SingleThreadExecutor should report false from the caller thread");
    require(future.get(), "SingleThreadExecutor should report true on its worker thread");
}

void singleThreadExecutorDoesNotRunNestedAddsInline()
{
    auto executor = std::make_shared<async::SingleThreadExecutor>();
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> order;
    bool done = false;

    executor->add([&]() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(1);
        }

        executor->add([&]() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(3);
                done = true;
            }
            cv.notify_one();
        });

        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(2);
        }
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
            return done;
        });
    }

    require(order.size() == 3, "nested add test should run all tasks");
    require(order[0] == 1 && order[1] == 2 && order[2] == 3,
            "SingleThreadExecutor should enqueue nested adds instead of running them inline");
}

void singleThreadExecutorRejectsTasksAfterShutdown()
{
    auto executor = std::make_shared<async::SingleThreadExecutor>();
    executor->shutdown();

    std::atomic<bool> ran(false);
    executor->add([&]() {
        ran = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(!ran.load(), "SingleThreadExecutor should reject tasks after shutdown");
}

void tryValueRethrowsStoredException()
{
    async::Try<int> result = async::Try<int>::fromException(
        std::make_exception_ptr(std::runtime_error("stored")));

    require(result.hasException(), "Try should report stored exception");
    requireThrows<std::runtime_error>([&]() {
        result.value();
    }, "Try::value should rethrow the stored exception");
}

void makeReadyFutureProducesValue()
{
    auto future = async::makeReadyFuture(42);
    require(future.isReady(), "makeReadyFuture should create a ready future");
    require(future.get() == 42, "makeReadyFuture should store the provided value");
}

void makeReadyFutureProducesUnit()
{
    auto future = async::makeReadyFuture();
    require(future.isReady(), "makeReadyFuture() should create a ready Unit future");
    async::Unit unit = future.get();
    (void)unit;
}

void makeExceptionFuturePropagatesException()
{
    auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("ready error")));

    require(future.isReady(), "makeExceptionFuture should create a ready future");
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "makeExceptionFuture should rethrow its stored exception");
}

void collectAllEmptyReturnsReadyEmptyVector()
{
    std::vector<async::Future<int>> futures;
    auto collected = async::collectAll(std::move(futures));

    require(collected.isReady(), "collectAll of an empty vector should be ready");
    require(collected.get().empty(), "collectAll of an empty vector should return an empty result vector");
}

void collectAllPreservesValuesAndExceptions()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(10));
    futures.push_back(async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("middle"))));
    futures.push_back(async::makeReadyFuture(32));

    auto collected = async::collectAll(std::move(futures));
    std::vector<async::Try<int>> results = collected.get();

    require(results.size() == 3, "collectAll should preserve result count");
    require(results[0].hasValue(), "collectAll result 0 should contain a value");
    require(results[0].value() == 10, "collectAll result 0 should preserve its value");
    require(results[1].hasException(), "collectAll result 1 should contain an exception");
    require(results[2].hasValue(), "collectAll result 2 should contain a value");
    require(results[2].value() == 32, "collectAll result 2 should preserve its value");

    requireThrows<std::runtime_error>([&]() {
        results[1].value();
    }, "collectAll should keep exceptions in their Try slot");
}

void collectAllWaitsForAsynchronousFutures()
{
    auto executor = std::make_shared<async::ThreadPoolExecutor>(2);
    std::vector<async::Future<int>> futures;
    futures.push_back(async::async(executor, []() {
        return 20;
    }));
    futures.push_back(async::async(executor, []() {
        return 22;
    }));

    auto collected = async::collectAll(std::move(futures));
    std::vector<async::Try<int>> results = collected.get();

    require(results.size() == 2, "collectAll should collect asynchronous futures");
    require(results[0].value() + results[1].value() == 42,
            "collectAll should preserve asynchronous values");
}

void collectAllRejectsInvalidFuture()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::Future<int>());

    auto collected = async::collectAll(std::move(futures));
    require(collected.isReady(), "collectAll should reject invalid futures immediately");
    requireThrows<async::FutureInvalid>([&]() {
        collected.get();
    }, "collectAll should complete with FutureInvalid when an input future is invalid");
}

void collectReturnsValuesWhenAllSucceed()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(10));
    futures.push_back(async::makeReadyFuture(32));

    auto collected = async::collect(std::move(futures));
    std::vector<int> values = collected.get();

    require(values.size() == 2, "collect should preserve value count");
    require(values[0] == 10 && values[1] == 32,
            "collect should preserve successful values in order");
}

void collectPropagatesFirstStoredException()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(1));
    futures.push_back(async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("collect failed"))));
    futures.push_back(async::makeReadyFuture(3));

    auto collected = async::collect(std::move(futures));
    requireThrows<std::runtime_error>([&]() {
        collected.get();
    }, "collect should fail when any input future fails");
}

void collectEmptyReturnsReadyEmptyVector()
{
    std::vector<async::Future<int>> futures;
    auto collected = async::collect(std::move(futures));

    require(collected.isReady(), "collect of an empty vector should be ready");
    require(collected.get().empty(), "collect of an empty vector should return an empty vector");
}

void collectAnyReturnsFirstSuccessfulCompletion()
{
    async::Promise<int> firstPromise;
    async::Promise<int> secondPromise;

    std::vector<async::Future<int>> futures;
    futures.push_back(firstPromise.getFuture());
    futures.push_back(secondPromise.getFuture());

    auto firstCompleted = async::collectAny(std::move(futures));
    secondPromise.setValue(42);

    auto result = firstCompleted.get();
    require(result.first == 1, "collectAny should report the first completed index");
    require(result.second.hasValue(), "collectAny should preserve a successful first result");
    require(result.second.value() == 42, "collectAny should preserve the first completed value");

    firstPromise.setValue(7);
}

void collectAnyReturnsFirstFailedCompletion()
{
    async::Promise<int> firstPromise;
    async::Promise<int> secondPromise;

    std::vector<async::Future<int>> futures;
    futures.push_back(firstPromise.getFuture());
    futures.push_back(secondPromise.getFuture());

    auto firstCompleted = async::collectAny(std::move(futures));
    firstPromise.setException(std::make_exception_ptr(std::runtime_error("first failed")));

    auto result = firstCompleted.get();
    require(result.first == 0, "collectAny should report a failed first completion index");
    require(result.second.hasException(), "collectAny should preserve a failed first result");
    requireThrows<std::runtime_error>([&]() {
        result.second.value();
    }, "collectAny should keep the first completion exception");

    secondPromise.setValue(42);
}

void collectAnyEmptyReturnsException()
{
    std::vector<async::Future<int>> futures;
    auto firstCompleted = async::collectAny(std::move(futures));

    require(firstCompleted.isReady(), "collectAny of an empty vector should be ready with an exception");
    requireThrows<async::FutureInvalid>([&]() {
        firstCompleted.get();
    }, "collectAny of an empty vector should fail");
}

void collectAnyRejectsInvalidFuture()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(1));
    futures.push_back(async::Future<int>());

    auto firstCompleted = async::collectAny(std::move(futures));
    require(firstCompleted.isReady(), "collectAny should reject invalid futures immediately");
    requireThrows<async::FutureInvalid>([&]() {
        firstCompleted.get();
    }, "collectAny should complete with FutureInvalid when an input future is invalid");
}

void collectAnyUsesFirstReadyFutureInInputOrder()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(10));
    futures.push_back(async::makeReadyFuture(32));

    auto firstCompleted = async::collectAny(std::move(futures));
    auto result = firstCompleted.get();

    require(result.first == 0, "collectAny should use the first ready future in input order");
    require(result.second.value() == 10, "collectAny should preserve the first ready value");
}

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

void withinRejectsInvalidFuture()
{
    auto future = async::within(async::Future<int>(), std::chrono::milliseconds(1));

    require(future.isReady(), "within should reject an invalid future immediately");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "within should complete with FutureInvalid for invalid input");
}

void futureSplitterFanoutBeforeCompletion()
{
    async::Promise<int> promise;
    async::FutureSplitter<int> splitter(promise.getFuture());

    auto first = splitter.getFuture();
    auto second = splitter.getFuture();

    require(!first.isReady(), "split future should wait for source completion");
    require(!second.isReady(), "second split future should wait for source completion");

    promise.setValue(42);
    require(first.get() == 42, "first split future should receive the source value");
    require(second.get() == 42, "second split future should receive the source value");
}

void futureSplitterReturnsReadyFutureAfterCompletion()
{
    async::Promise<int> promise;
    async::FutureSplitter<int> splitter(promise.getFuture());
    auto first = splitter.getFuture();

    promise.setValue(42);
    require(first.get() == 42, "initial split future should receive source value");

    auto late = splitter.getFuture();
    require(late.isReady(), "split future requested after source completion should be ready");
    require(late.get() == 42, "late split future should receive stored value");
}

void futureSplitterBroadcastsException()
{
    async::Promise<int> promise;
    async::FutureSplitter<int> splitter(promise.getFuture());

    auto first = splitter.getFuture();
    auto second = splitter.getFuture();

    promise.setException(std::make_exception_ptr(std::runtime_error("split error")));

    requireThrows<std::runtime_error>([&]() {
        first.get();
    }, "first split future should receive source exception");
    requireThrows<std::runtime_error>([&]() {
        second.get();
    }, "second split future should receive source exception");

    auto late = splitter.getFuture();
    requireThrows<std::runtime_error>([&]() {
        late.get();
    }, "late split future should receive stored source exception");
}

void futureSplitterRejectsInvalidSource()
{
    async::FutureSplitter<int> splitter{ async::Future<int>() };

    auto future = splitter.getFuture();
    require(future.isReady(), "splitter from invalid source should return ready exception futures");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "splitter from invalid source should fail with FutureInvalid");
}

void futureSplitterSupportsUnit()
{
    async::Promise<async::Unit> promise;
    async::FutureSplitter<async::Unit> splitter(promise.getFuture());

    auto first = splitter.getFuture();
    auto second = splitter.getFuture();

    promise.setValue();

    async::Unit firstUnit = first.get();
    async::Unit secondUnit = second.get();
    (void)firstUnit;
    (void)secondUnit;

    auto late = splitter.getFuture();
    require(late.isReady(), "late Unit split future should be ready");
    async::Unit lateUnit = late.get();
    (void)lateUnit;
}

using TestCase = std::pair<const char *, std::function<void()>>;

std::vector<TestCase> testCases()
{
    return {
        { "promiseCanBeFulfilledBeforeFutureRetrieval", promiseCanBeFulfilledBeforeFutureRetrieval },
        { "promiseRejectsDuplicateFutureRetrieval", promiseRejectsDuplicateFutureRetrieval },
        { "promiseRejectsDuplicateFulfillment", promiseRejectsDuplicateFulfillment },
        { "futureCannotBeUsedAfterGet", futureCannotBeUsedAfterGet },
        { "futureCannotBeUsedAfterThenValue", futureCannotBeUsedAfterThenValue },
        { "brokenPromiseCompletesFutureWithException", brokenPromiseCompletesFutureWithException },
        { "cancelAfterHandlerNotifiesProducer", cancelAfterHandlerNotifiesProducer },
        { "cancelBeforeHandlerIsDeliveredWhenHandlerIsSet", cancelBeforeHandlerIsDeliveredWhenHandlerIsSet },
        { "raiseDeliversCustomReason", raiseDeliversCustomReason },
        { "cancelAfterFulfilledDoesNothing", cancelAfterFulfilledDoesNothing },
        { "interruptHandlerRunsOutsideSharedStateLock", interruptHandlerRunsOutsideSharedStateLock },
        { "thenValueTransformsSuccessfulResult", thenValueTransformsSuccessfulResult },
        { "thenValuePropagatesExceptionToThenError", thenValuePropagatesExceptionToThenError },
        { "thenTryObservesValueAndException", thenTryObservesValueAndException },
        { "readyFutureRunsContinuationWhenAttached", readyFutureRunsContinuationWhenAttached },
        { "thenValueFlattensReturnedFuture", thenValueFlattensReturnedFuture },
        { "thenTryFlattensReturnedFuture", thenTryFlattensReturnedFuture },
        { "thenErrorFlattensReturnedFuture", thenErrorFlattensReturnedFuture },
        { "flattenedFuturePropagatesInnerException", flattenedFuturePropagatesInnerException },
        { "unitFutureSupportsVoidProducerAndContinuation", unitFutureSupportsVoidProducerAndContinuation },
        { "nullExecutorFallsBackToInlineExecution", nullExecutorFallsBackToInlineExecution },
        { "continuationRunsThroughViaExecutor", continuationRunsThroughViaExecutor },
        { "asyncRunsOnThreadPoolExecutor", asyncRunsOnThreadPoolExecutor },
        { "asyncRunsOnThreadExecutor", asyncRunsOnThreadExecutor },
        { "manualExecutorQueuesUntilDrained", manualExecutorQueuesUntilDrained },
        { "serialExecutorRunsQueuedTasksInOrderOnDrain", serialExecutorRunsQueuedTasksInOrderOnDrain },
        { "serialExecutorDoesNotRunNestedAddsReentrantly", serialExecutorDoesNotRunNestedAddsReentrantly },
        { "serialExecutorRejectsAfterShutdown", serialExecutorRejectsAfterShutdown },
        { "singleThreadExecutorRunsTasksOnOneThreadInFifoOrder", singleThreadExecutorRunsTasksOnOneThreadInFifoOrder },
        { "singleThreadExecutorReportsExecutorThread", singleThreadExecutorReportsExecutorThread },
        { "singleThreadExecutorDoesNotRunNestedAddsInline", singleThreadExecutorDoesNotRunNestedAddsInline },
        { "singleThreadExecutorRejectsTasksAfterShutdown", singleThreadExecutorRejectsTasksAfterShutdown },
        { "tryValueRethrowsStoredException", tryValueRethrowsStoredException },
        { "makeReadyFutureProducesValue", makeReadyFutureProducesValue },
        { "makeReadyFutureProducesUnit", makeReadyFutureProducesUnit },
        { "makeExceptionFuturePropagatesException", makeExceptionFuturePropagatesException },
        { "collectAllEmptyReturnsReadyEmptyVector", collectAllEmptyReturnsReadyEmptyVector },
        { "collectAllPreservesValuesAndExceptions", collectAllPreservesValuesAndExceptions },
        { "collectAllWaitsForAsynchronousFutures", collectAllWaitsForAsynchronousFutures },
        { "collectAllRejectsInvalidFuture", collectAllRejectsInvalidFuture },
        { "collectReturnsValuesWhenAllSucceed", collectReturnsValuesWhenAllSucceed },
        { "collectPropagatesFirstStoredException", collectPropagatesFirstStoredException },
        { "collectEmptyReturnsReadyEmptyVector", collectEmptyReturnsReadyEmptyVector },
        { "collectAnyReturnsFirstSuccessfulCompletion", collectAnyReturnsFirstSuccessfulCompletion },
        { "collectAnyReturnsFirstFailedCompletion", collectAnyReturnsFirstFailedCompletion },
        { "collectAnyEmptyReturnsException", collectAnyEmptyReturnsException },
        { "collectAnyRejectsInvalidFuture", collectAnyRejectsInvalidFuture },
        { "collectAnyUsesFirstReadyFutureInInputOrder", collectAnyUsesFirstReadyFutureInInputOrder },
        { "sleepForCompletesAfterDuration", sleepForCompletesAfterDuration },
        { "sleepUntilCompletesAtDeadline", sleepUntilCompletesAtDeadline },
        { "withinReturnsOriginalValueBeforeTimeout", withinReturnsOriginalValueBeforeTimeout },
        { "withinPropagatesOriginalExceptionBeforeTimeout", withinPropagatesOriginalExceptionBeforeTimeout },
        { "withinTimesOutWhenDeadlineWins", withinTimesOutWhenDeadlineWins },
        { "withinIgnoresLateOriginalCompletion", withinIgnoresLateOriginalCompletion },
        { "withinRejectsInvalidFuture", withinRejectsInvalidFuture },
        { "futureSplitterFanoutBeforeCompletion", futureSplitterFanoutBeforeCompletion },
        { "futureSplitterReturnsReadyFutureAfterCompletion", futureSplitterReturnsReadyFutureAfterCompletion },
        { "futureSplitterBroadcastsException", futureSplitterBroadcastsException },
        { "futureSplitterRejectsInvalidSource", futureSplitterRejectsInvalidSource },
        { "futureSplitterSupportsUnit", futureSplitterSupportsUnit },
    };
}
}

int main()
{
    int failedCount = 0;
    for (const TestCase &test : testCases()) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception &e) {
            ++failedCount;
            std::cerr << "[FAIL] " << test.first << ": " << e.what() << '\n';
        } catch (...) {
            ++failedCount;
            std::cerr << "[FAIL] " << test.first << ": unknown exception\n";
        }
    }

    if (failedCount != 0) {
        std::cerr << failedCount << " async future test(s) failed.\n";
        return 1;
    }

    std::cout << "All async future tests passed.\n";
    return 0;
}
