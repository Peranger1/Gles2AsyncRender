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
class RejectingExecutor final : public async::Executor
{
public:
    bool add(std::function<void()> task) override
    {
        (void)task;
        ++addCount;
        return false;
    }

    bool isShutdown() const override
    {
        return true;
    }

    const char *typeName() const override
    {
        return "RejectingExecutor";
    }

    int addCount = 0;
};

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

void asyncRejectedExecutorCompletesWithExecutorRejected()
{
    auto executor = std::make_shared<RejectingExecutor>();
    auto future = async::async(executor, []() {
        return 42;
    });

    require(executor->addCount == 1, "async should attempt to schedule once");
    require(future.isReady(), "async should complete immediately when executor rejects");
    requireThrows<async::ExecutorRejected>([&]() {
        future.get();
    }, "async should surface executor rejection");
}

void continuationRejectedByViaExecutorCompletesDownstream()
{
    auto executor = std::make_shared<RejectingExecutor>();
    async::Promise<int> promise;
    bool ran = false;

    auto future = std::move(promise.getFuture())
        .via(executor)
        .thenValue([&](int value) {
            ran = true;
            return value + 1;
        });

    promise.setValue(41);

    require(!ran, "rejected continuation should not run user callback");
    require(future.isReady(), "rejected continuation should complete downstream future");
    requireThrows<async::ExecutorRejected>([&]() {
        future.get();
    }, "rejected continuation should surface ExecutorRejected");
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

void asyncFlattensReturnedFuture()
{
    auto executor = async::InlineExecutor::instance();
    auto innerPromise = std::make_shared<async::Promise<int>>();
    auto future = async::async(executor, [innerPromise]() {
        return innerPromise->getFuture();
    });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "async should flatten returned Future<T>");
    require(!future.isReady(), "async should wait for a pending returned future");

    innerPromise->setValue(42);
    require(future.get() == 42, "async should complete with the returned future value");
}

void asyncFlattensReadyReturnedFuture()
{
    auto executor = async::InlineExecutor::instance();
    auto future = async::async(executor, []() {
        return async::makeReadyFuture(42);
    });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "async should flatten ready returned Future<T>");
    require(future.isReady(), "async should complete immediately for a ready returned future");
    require(future.get() == 42, "async should preserve a ready returned future value");
}

void asyncPropagatesReturnedFutureException()
{
    auto executor = async::InlineExecutor::instance();
    auto innerPromise = std::make_shared<async::Promise<int>>();
    auto future = async::async(executor, [innerPromise]() {
        return innerPromise->getFuture();
    });

    require(!future.isReady(), "async should wait for a pending returned exception future");

    innerPromise->setException(std::make_exception_ptr(std::runtime_error("inner")));
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "async should propagate returned future exceptions");
}

void asyncRejectsInvalidReturnedFuture()
{
    auto executor = async::InlineExecutor::instance();
    auto future = async::async(executor, []() {
        return async::Future<int>();
    });

    static_assert(std::is_same<decltype(future), async::Future<int>>::value,
                  "async should flatten invalid returned Future<T> type");
    require(future.isReady(), "async should complete immediately for an invalid returned future");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "async should surface FutureInvalid for an invalid returned future");
}

void asyncFlattensReturnedUnitFuture()
{
    auto executor = async::InlineExecutor::instance();
    auto future = async::async(executor, []() {
        return async::makeReadyFuture();
    });

    static_assert(std::is_same<decltype(future), async::Future<async::Unit>>::value,
                  "async should flatten returned Future<Unit>");
    require(future.isReady(), "async should complete immediately for a ready returned Unit future");
    async::Unit unit = future.get();
    (void)unit;
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
    const bool accepted = executor.add([&]() {
        ran = true;
    });

    underlying->drain();
    require(!accepted, "SerialExecutor::add should report rejection after shutdown");
    require(executor.isShutdown(), "SerialExecutor should report shutdown after shutdown");
    require(!ran, "SerialExecutor should reject tasks after shutdown");
}

void serialExecutorRejectsWhenUnderlyingRejects()
{
    auto underlying = std::make_shared<RejectingExecutor>();
    async::SerialExecutor executor(underlying);

    bool ran = false;
    const bool accepted = executor.add([&]() {
        ran = true;
    });

    require(!accepted, "SerialExecutor should report rejection from underlying executor");
    require(!ran, "SerialExecutor should not run tasks rejected by underlying executor");
    require(executor.isShutdown(), "SerialExecutor should shut down after underlying rejection");
    require(executor.queuedCount() == 0, "SerialExecutor should clear queued tasks after underlying rejection");
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
    const bool accepted = executor->add([&]() {
        ran = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(!accepted, "SingleThreadExecutor::add should report rejection after shutdown");
    require(executor->isShutdown(), "SingleThreadExecutor should report shutdown after shutdown");
    require(!ran.load(), "SingleThreadExecutor should reject tasks after shutdown");
}

void executorObservabilityReportsTypesAndQueues()
{
    require(std::string(async::InlineExecutor::instance()->typeName()) == "InlineExecutor",
            "InlineExecutor should report its type name");

    async::ThreadExecutor threadExecutor;
    require(std::string(threadExecutor.typeName()) == "ThreadExecutor",
            "ThreadExecutor should report its type name");

    auto manual = std::make_shared<async::ManualExecutor>();
    bool manualRan = false;
    require(manual->add([&]() {
        manualRan = true;
    }), "ManualExecutor should accept a valid task");
    require(manual->queuedCount() == 1, "ManualExecutor should report queued tasks before drain");
    manual->drain();
    require(manualRan, "ManualExecutor should run drained tasks");
    require(manual->queuedCount() == 0, "ManualExecutor should report no queued tasks after drain");

    auto underlying = std::make_shared<async::ManualExecutor>();
    async::SerialExecutor serial(underlying);
    bool serialRan = false;
    require(std::string(serial.typeName()) == "SerialExecutor",
            "SerialExecutor should report its type name");
    require(serial.add([&]() {
        serialRan = true;
    }), "SerialExecutor should accept a task while active");
    require(serial.queuedCount() == 1, "SerialExecutor should report its internal queued task");
    underlying->drain();
    require(serialRan, "SerialExecutor should run queued tasks through the underlying executor");
    require(serial.queuedCount() == 0, "SerialExecutor should report no queued tasks after drain");

    std::mutex mutex;
    std::condition_variable cv;
    bool firstStarted = false;
    bool releaseFirst = false;
    bool secondRan = false;
    async::ThreadPoolExecutor pool(1);
    require(std::string(pool.typeName()) == "ThreadPoolExecutor",
            "ThreadPoolExecutor should report its type name");
    require(pool.add([&]() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            firstStarted = true;
        }
        cv.notify_one();

        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
            return releaseFirst;
        });
    }), "ThreadPoolExecutor should accept a blocking task");

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
            return firstStarted;
        });
    }

    require(pool.add([&]() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            secondRan = true;
        }
        cv.notify_one();
    }), "ThreadPoolExecutor should accept a queued task");
    require(pool.queuedCount() >= 1, "ThreadPoolExecutor should report pending queued tasks");

    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseFirst = true;
    }
    cv.notify_one();

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
            return secondRan;
        });
    }

    async::SingleThreadExecutor single("observability");
    require(std::string(single.typeName()) == "SingleThreadExecutor",
            "SingleThreadExecutor should report its type name");
    require(!single.isShutdown(), "SingleThreadExecutor should report running before shutdown");
    single.shutdown();
    require(single.isShutdown(), "SingleThreadExecutor should report shutdown after shutdown");
}
}

namespace async_future_test
{
std::vector<execution_test::TestCase> executorTestCases()
{
    return {
        { "nullExecutorFallsBackToInlineExecution", nullExecutorFallsBackToInlineExecution },
        { "continuationRunsThroughViaExecutor", continuationRunsThroughViaExecutor },
        { "asyncRejectedExecutorCompletesWithExecutorRejected", asyncRejectedExecutorCompletesWithExecutorRejected },
        { "continuationRejectedByViaExecutorCompletesDownstream", continuationRejectedByViaExecutorCompletesDownstream },
        { "asyncRunsOnThreadPoolExecutor", asyncRunsOnThreadPoolExecutor },
        { "asyncRunsOnThreadExecutor", asyncRunsOnThreadExecutor },
        { "asyncFlattensReturnedFuture", asyncFlattensReturnedFuture },
        { "asyncFlattensReadyReturnedFuture", asyncFlattensReadyReturnedFuture },
        { "asyncPropagatesReturnedFutureException", asyncPropagatesReturnedFutureException },
        { "asyncRejectsInvalidReturnedFuture", asyncRejectsInvalidReturnedFuture },
        { "asyncFlattensReturnedUnitFuture", asyncFlattensReturnedUnitFuture },
        { "manualExecutorQueuesUntilDrained", manualExecutorQueuesUntilDrained },
        { "serialExecutorRunsQueuedTasksInOrderOnDrain", serialExecutorRunsQueuedTasksInOrderOnDrain },
        { "serialExecutorDoesNotRunNestedAddsReentrantly", serialExecutorDoesNotRunNestedAddsReentrantly },
        { "serialExecutorRejectsAfterShutdown", serialExecutorRejectsAfterShutdown },
        { "serialExecutorRejectsWhenUnderlyingRejects", serialExecutorRejectsWhenUnderlyingRejects },
        { "singleThreadExecutorRunsTasksOnOneThreadInFifoOrder", singleThreadExecutorRunsTasksOnOneThreadInFifoOrder },
        { "singleThreadExecutorReportsExecutorThread", singleThreadExecutorReportsExecutorThread },
        { "singleThreadExecutorDoesNotRunNestedAddsInline", singleThreadExecutorDoesNotRunNestedAddsInline },
        { "singleThreadExecutorRejectsTasksAfterShutdown", singleThreadExecutorRejectsTasksAfterShutdown },
        { "executorObservabilityReportsTypesAndQueues", executorObservabilityReportsTypesAndQueues },
    };
}
}
