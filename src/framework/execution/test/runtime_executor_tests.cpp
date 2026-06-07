#include "framework/execution/runtime_executor.h"
#include "framework/execution/test/test_harness.h"

#include <QString>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using execution_test::require;
using execution_test::requireThrows;

// 本文件覆盖 RuntimeExecutor 的线程固定、幂等、失败传播和 submitBlocking 边界。

struct RuntimeEvents final
{
    std::thread::id initializeThread;
    std::thread::id enterThread;
    std::thread::id leaveThread;
    std::thread::id shutdownThread;
    int initializeCount = 0;
    int enterCount = 0;
    int leaveCount = 0;
    int shutdownCount = 0;
};

class FakeRuntime final : public IRuntime
{
public:
    explicit FakeRuntime(std::shared_ptr<RuntimeEvents> events,
                         bool initializeResult = true)
        : m_events(std::move(events))
        , m_initializeResult(initializeResult)
    {
    }

    explicit FakeRuntime(bool initializeResult = true)
        : FakeRuntime(std::make_shared<RuntimeEvents>(), initializeResult)
    {
    }

    bool initialize(QString *error) override
    {
        m_events->initializeThread = std::this_thread::get_id();
        ++m_events->initializeCount;
        if (!m_initializeResult) {
            if (error) {
                *error = QStringLiteral("fake init failed");
            }
            return false;
        }
        return true;
    }

    bool enter(QString *) override
    {
        m_events->enterThread = std::this_thread::get_id();
        ++m_events->enterCount;
        return true;
    }

    void leave() override
    {
        m_events->leaveThread = std::this_thread::get_id();
        ++m_events->leaveCount;
    }

    void shutdown() override
    {
        m_events->shutdownThread = std::this_thread::get_id();
        ++m_events->shutdownCount;
    }

    void *resolveProc(const char *) const override
    {
        return nullptr;
    }

private:
    std::shared_ptr<RuntimeEvents> m_events;
    bool m_initializeResult = true;
};

void initializeRunsOnRuntimeThread()
{
    // 验证 initialize 被投递到 runtime executor，而不是调用方线程。
    auto events = std::make_shared<RuntimeEvents>();
    auto runtime = std::make_unique<FakeRuntime>(events);
    auto executor = std::make_shared<async::SingleThreadExecutor>("runtime-test");
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime), executor);

    runtimeExecutor.initialize().get();

    require(events->initializeCount == 1, "RuntimeExecutor should initialize runtime once");
    require(events->initializeThread != std::this_thread::get_id(),
            "RuntimeExecutor should initialize on executor thread");
    require(executor->isOnExecutorThread() == false,
            "caller should not be on runtime executor thread");

    runtimeExecutor.shutdown().get();
}

void initializeFailureBecomesFutureException()
{
    auto runtime = std::make_unique<FakeRuntime>(false);
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));

    auto future = runtimeExecutor.initialize();
    requireThrows<execution::RuntimeInitializeFailed>([&]() {
        future.get();
    }, "RuntimeExecutor should surface initialize failure");
}

void initializeIsIdempotent()
{
    auto events = std::make_shared<RuntimeEvents>();
    auto runtime = std::make_unique<FakeRuntime>(events);
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));

    runtimeExecutor.initialize().get();
    runtimeExecutor.initialize().get();

    require(events->initializeCount == 1, "RuntimeExecutor should initialize runtime only once");
    runtimeExecutor.shutdown().get();
}

void submitRunsOnRuntimeThreadAndReturnsValue()
{
    auto events = std::make_shared<RuntimeEvents>();
    auto runtime = std::make_unique<FakeRuntime>(events);
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([](IRuntime &runtime) {
        QString error;
        runtime.enter(&error);
        runtime.leave();
        return 42;
    });

    require(future.get() == 42, "RuntimeExecutor should return submitted task value");
    require(events->enterCount == 1, "RuntimeExecutor should run task against runtime");
    require(events->enterThread == events->initializeThread,
            "RuntimeExecutor should run submitted task on initialize thread");
    require(events->leaveThread == events->initializeThread,
            "RuntimeExecutor should leave on initialize thread");

    runtimeExecutor.shutdown().get();
}

void submitFlattensReturnedFuture()
{
    // 验证 submit 的 Future<T> 返回值会 flatten，且等待 pending inner future。
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto innerPromise = std::make_shared<async::Promise<int>>();
    std::mutex mutex;
    std::condition_variable cv;
    bool submitted = false;

    auto future = runtimeExecutor.submit([innerPromise, &mutex, &cv, &submitted](IRuntime &) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            submitted = true;
        }
        cv.notify_one();
        return innerPromise->getFuture();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
            return submitted;
        });
    }

    require(!future.isReady(), "RuntimeExecutor should wait for pending returned futures");
    innerPromise->setValue(42);
    require(future.get() == 42, "RuntimeExecutor should flatten returned futures");

    runtimeExecutor.shutdown().get();
}

void submitPropagatesReturnedFutureException()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([](IRuntime &) {
        return async::makeExceptionFuture<int>(std::make_exception_ptr(std::runtime_error("returned")));
    });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "RuntimeExecutor should propagate returned future exceptions");

    runtimeExecutor.shutdown().get();
}

void submitRejectsInvalidReturnedFuture()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([](IRuntime &) {
        return async::Future<int>();
    });

    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "RuntimeExecutor should reject invalid returned futures");

    runtimeExecutor.shutdown().get();
}

void submitCapturesTaskException()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));

    auto future = runtimeExecutor.submit([](IRuntime &) -> int {
        throw std::runtime_error("runtime task failed");
    });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "RuntimeExecutor should capture submitted task exceptions");
}

void submitRejectsAfterShutdown()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.shutdown().get();

    auto future = runtimeExecutor.submit([](IRuntime &) {
        return 42;
    });

    requireThrows<execution::ExecutorShutdown>([&]() {
        future.get();
    }, "RuntimeExecutor should reject submit after shutdown");
}

void submitBlockingRunsInlineOnRuntimeThread()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([&runtimeExecutor](IRuntime &) {
        return runtimeExecutor.submitBlocking([](IRuntime &) {
            return 42;
        });
    });

    require(future.get() == 42, "submitBlocking should run inline on the runtime thread");
    runtimeExecutor.shutdown().get();
}

void submitBlockingFlattensReturnedFutureFromCaller()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    const int value = runtimeExecutor.submitBlocking([](IRuntime &) {
        return async::makeReadyFuture(42);
    });

    require(value == 42, "submitBlocking should flatten returned futures from callers");
    runtimeExecutor.shutdown().get();
}

void submitBlockingFlattensReturnedFutureOnRuntimeThread()
{
    // 验证 runtime 线程 inline 分支也遵守 Future<T> flatten 语义。
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([&runtimeExecutor](IRuntime &) {
        return runtimeExecutor.submitBlocking([](IRuntime &) {
            return async::makeReadyFuture(42);
        });
    });

    require(future.get() == 42,
            "submitBlocking should flatten returned futures on the runtime thread");
    runtimeExecutor.shutdown().get();
}

void submitBlockingPropagatesReturnedFutureExceptionOnRuntimeThread()
{
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([&runtimeExecutor](IRuntime &) {
        return runtimeExecutor.submitBlocking([](IRuntime &) {
            return async::makeExceptionFuture<int>(
                std::make_exception_ptr(std::runtime_error("blocking returned")));
        });
    });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "submitBlocking should propagate returned future exceptions on the runtime thread");

    runtimeExecutor.shutdown().get();
}

void submitBlockingRejectsInvalidReturnedFutureOnRuntimeThread()
{
    // 验证 inline 分支返回 invalid future 时，错误会被外层 submit 捕获。
    auto runtime = std::make_unique<FakeRuntime>();
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    auto future = runtimeExecutor.submit([&runtimeExecutor](IRuntime &) {
        return runtimeExecutor.submitBlocking([](IRuntime &) {
            return async::Future<int>();
        });
    });

    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "submitBlocking should reject invalid returned futures on the runtime thread");

    runtimeExecutor.shutdown().get();
}

void shutdownRunsOnRuntimeThread()
{
    auto events = std::make_shared<RuntimeEvents>();
    auto runtime = std::make_unique<FakeRuntime>(events);
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    const std::thread::id initializeThread = events->initializeThread;
    runtimeExecutor.shutdown().get();

    require(events->shutdownCount == 1, "RuntimeExecutor should call runtime shutdown once");
    require(events->shutdownThread == initializeThread,
            "RuntimeExecutor should shutdown runtime on executor thread");
}

void shutdownIsIdempotent()
{
    auto events = std::make_shared<RuntimeEvents>();
    auto runtime = std::make_unique<FakeRuntime>(events);
    execution::RuntimeExecutor runtimeExecutor(std::move(runtime));
    runtimeExecutor.initialize().get();

    runtimeExecutor.shutdown().get();
    runtimeExecutor.shutdown().get();

    require(events->shutdownCount == 1, "RuntimeExecutor should shutdown runtime only once");
}

std::vector<execution_test::TestCase> testCases()
{
    return {
        { "initializeRunsOnRuntimeThread", initializeRunsOnRuntimeThread },
        { "initializeFailureBecomesFutureException", initializeFailureBecomesFutureException },
        { "initializeIsIdempotent", initializeIsIdempotent },
        { "submitRunsOnRuntimeThreadAndReturnsValue", submitRunsOnRuntimeThreadAndReturnsValue },
        { "submitFlattensReturnedFuture", submitFlattensReturnedFuture },
        { "submitPropagatesReturnedFutureException", submitPropagatesReturnedFutureException },
        { "submitRejectsInvalidReturnedFuture", submitRejectsInvalidReturnedFuture },
        { "submitCapturesTaskException", submitCapturesTaskException },
        { "submitRejectsAfterShutdown", submitRejectsAfterShutdown },
        { "submitBlockingRunsInlineOnRuntimeThread", submitBlockingRunsInlineOnRuntimeThread },
        { "submitBlockingFlattensReturnedFutureFromCaller",
          submitBlockingFlattensReturnedFutureFromCaller },
        { "submitBlockingFlattensReturnedFutureOnRuntimeThread",
          submitBlockingFlattensReturnedFutureOnRuntimeThread },
        { "submitBlockingPropagatesReturnedFutureExceptionOnRuntimeThread",
          submitBlockingPropagatesReturnedFutureExceptionOnRuntimeThread },
        { "submitBlockingRejectsInvalidReturnedFutureOnRuntimeThread",
          submitBlockingRejectsInvalidReturnedFutureOnRuntimeThread },
        { "shutdownRunsOnRuntimeThread", shutdownRunsOnRuntimeThread },
        { "shutdownIsIdempotent", shutdownIsIdempotent },
    };
}
}

int main()
{
    return execution_test::runTests(testCases(),
                                    "runtime executor",
                                    "All runtime executor tests passed.");
}
