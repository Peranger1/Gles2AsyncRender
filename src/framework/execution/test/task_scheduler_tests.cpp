#include "execution_test_cases.h"

#include "framework/execution/execution.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using execution_test::require;
using execution_test::requireThrows;

void taskSchedulerRunsSubmittedTask()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());

    auto future = scheduler.submit([]() {
        return 42;
    });

    require(future.get() == 42, "TaskScheduler should complete with submitted task value");
}

void taskSchedulerCompletesVoidTask()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());
    bool ran = false;

    auto future = scheduler.submit([&]() {
        ran = true;
    });

    future.get();
    require(ran, "TaskScheduler should complete void tasks");
}

void taskSchedulerCapturesThrownException()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());

    auto future = scheduler.submit([]() -> int {
        throw std::runtime_error("scheduler error");
    });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "TaskScheduler should capture task exceptions");
}

void taskSchedulerRejectsAfterShutdown()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());
    scheduler.shutdown();

    auto future = scheduler.submit([]() {
        return 42;
    });

    requireThrows<execution::ExecutorShutdown>([&]() {
        future.get();
    }, "TaskScheduler should reject submissions after shutdown");
}

void taskSchedulerRejectsMissingExecutor()
{
    execution::TaskScheduler scheduler(nullptr);

    auto future = scheduler.submit([]() {
        return 42;
    });

    requireThrows<execution::TaskRejected>([&]() {
        future.get();
    }, "TaskScheduler should reject missing executor");
}

void taskSchedulerFlattensReturnedFuture()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());

    auto future = scheduler.submit([]() {
        return async::makeReadyFuture(42);
    });

    require(future.get() == 42, "TaskScheduler should flatten returned futures");
}

void taskSchedulerFlattensPendingReturnedFuture()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());
    auto promise = std::make_shared<async::Promise<int>>();

    auto future = scheduler.submit([promise]() {
        return promise->getFuture();
    });

    require(!future.isReady(), "TaskScheduler should wait for pending returned futures");

    promise->setValue(42);
    require(future.get() == 42, "TaskScheduler should complete with pending returned future values");
}

void taskSchedulerFlattensReturnedUnitFuture()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());
    auto promise = std::make_shared<async::Promise<async::Unit>>();

    auto future = scheduler.submit([promise]() {
        return promise->getFuture();
    });

    require(!future.isReady(), "TaskScheduler should wait for pending returned unit futures");

    promise->setValue();
    future.get();
}

void taskSchedulerPropagatesReturnedFutureException()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());

    auto future = scheduler.submit([]() {
        return async::makeExceptionFuture<int>(std::make_exception_ptr(std::runtime_error("returned")));
    });

    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "TaskScheduler should propagate returned future exceptions");
}

void taskSchedulerRejectsInvalidReturnedFuture()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());

    auto future = scheduler.submit([]() {
        return async::Future<int>();
    });

    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "TaskScheduler should reject invalid returned futures");
}
}

namespace execution_component_test
{
std::vector<execution_test::TestCase> taskSchedulerTestCases()
{
    return {
        { "taskSchedulerRunsSubmittedTask", taskSchedulerRunsSubmittedTask },
        { "taskSchedulerCompletesVoidTask", taskSchedulerCompletesVoidTask },
        { "taskSchedulerCapturesThrownException", taskSchedulerCapturesThrownException },
        { "taskSchedulerRejectsAfterShutdown", taskSchedulerRejectsAfterShutdown },
        { "taskSchedulerRejectsMissingExecutor", taskSchedulerRejectsMissingExecutor },
        { "taskSchedulerFlattensReturnedFuture", taskSchedulerFlattensReturnedFuture },
        { "taskSchedulerFlattensPendingReturnedFuture", taskSchedulerFlattensPendingReturnedFuture },
        { "taskSchedulerFlattensReturnedUnitFuture", taskSchedulerFlattensReturnedUnitFuture },
        { "taskSchedulerPropagatesReturnedFutureException", taskSchedulerPropagatesReturnedFutureException },
        { "taskSchedulerRejectsInvalidReturnedFuture", taskSchedulerRejectsInvalidReturnedFuture },
    };
}
}
