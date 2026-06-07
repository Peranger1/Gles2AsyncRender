#include "framework/execution/runtime_executor.h"

#include <QString>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
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

void initializeRunsOnRuntimeThread()
{
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

using TestCase = std::pair<const char *, std::function<void()>>;

std::vector<TestCase> testCases()
{
    return {
        { "initializeRunsOnRuntimeThread", initializeRunsOnRuntimeThread },
        { "initializeFailureBecomesFutureException", initializeFailureBecomesFutureException },
        { "submitRunsOnRuntimeThreadAndReturnsValue", submitRunsOnRuntimeThreadAndReturnsValue },
        { "submitCapturesTaskException", submitCapturesTaskException },
        { "submitRejectsAfterShutdown", submitRejectsAfterShutdown },
        { "shutdownRunsOnRuntimeThread", shutdownRunsOnRuntimeThread },
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
        std::cerr << failedCount << " runtime executor test(s) failed.\n";
        return 1;
    }

    std::cout << "All runtime executor tests passed.\n";
    return 0;
}
