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

// 本文件覆盖 SerialLane 的 FIFO、runner 失败和 shutdown 等串行队列行为。

void serialLaneRunsOneTaskAtATimeInOrder()
{
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::SerialLane<int, int> lane([&](int value) {
        started.push_back(value);
        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);

    require(started.size() == 1 && started[0] == 1,
            "SerialLane should start only the first task while active");

    completions[0]->setValue(10);
    require(first.get() == 10, "SerialLane should complete the first future");
    require(started.size() == 2 && started[1] == 2,
            "SerialLane should start the second task after the first completes");

    completions[1]->setValue(20);
    require(second.get() == 20, "SerialLane should complete the second future");
}

void serialLanePropagatesRunnerFailure()
{
    execution::SerialLane<int, int> lane([](int) {
        return async::makeExceptionFuture<int>(std::make_exception_ptr(std::runtime_error("runner failed")));
    });

    auto future = lane.submit(1);
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "SerialLane should propagate failed runner futures");
}

void serialLaneRunnerThrowStartsNextWaitingTask()
{
    // 验证 runner 抛异常后 active 状态会释放，后续 waiting 仍能继续执行。
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::SerialLane<int, int> lane([&](int value) {
        started.push_back(value);
        if (value == 2) {
            throw std::runtime_error("runner threw");
        }

        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    completions[0]->setValue(10);
    require(first.get() == 10, "SerialLane should complete the active task before runner throw");
    requireThrows<std::runtime_error>([&]() {
        second.get();
    }, "SerialLane should fail the throwing runner task");
    require(started.size() == 3 && started[0] == 1 && started[1] == 2 && started[2] == 3,
            "SerialLane should continue to the next waiting task after runner throw");

    completions[1]->setValue(30);
    require(third.get() == 30, "SerialLane should complete the task after a throwing runner");
}

void serialLaneInvalidRunnerFutureStartsNextWaitingTask()
{
    // 验证 runner 返回 invalid future 后也不会卡住 lane。
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::SerialLane<int, int> lane([&](int value) {
        started.push_back(value);
        if (value == 2) {
            return async::Future<int>();
        }

        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    completions[0]->setValue(10);
    require(first.get() == 10, "SerialLane should complete the active task before invalid runner future");
    requireThrows<async::FutureInvalid>([&]() {
        second.get();
    }, "SerialLane should fail an invalid runner future");
    require(started.size() == 3 && started[0] == 1 && started[1] == 2 && started[2] == 3,
            "SerialLane should continue to the next waiting task after invalid runner future");

    completions[1]->setValue(30);
    require(third.get() == 30, "SerialLane should complete the task after an invalid runner future");
}

void serialLaneShutdownRejectsWaitingTasks()
{
    auto activePromise = std::make_shared<async::Promise<int>>();
    execution::SerialLane<int, int> lane([&](int value) {
        if (value == 1) {
            return activePromise->getFuture();
        }
        return async::makeReadyFuture(value);
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    lane.shutdown();

    requireThrows<execution::ExecutorShutdown>([&]() {
        second.get();
    }, "SerialLane shutdown should reject waiting tasks");

    activePromise->setValue(10);
    require(first.get() == 10, "SerialLane shutdown should not cancel the active task");
}
}

namespace execution_component_test
{
std::vector<execution_test::TestCase> serialLaneTestCases()
{
    return {
        { "serialLaneRunsOneTaskAtATimeInOrder", serialLaneRunsOneTaskAtATimeInOrder },
        { "serialLanePropagatesRunnerFailure", serialLanePropagatesRunnerFailure },
        { "serialLaneRunnerThrowStartsNextWaitingTask", serialLaneRunnerThrowStartsNextWaitingTask },
        { "serialLaneInvalidRunnerFutureStartsNextWaitingTask", serialLaneInvalidRunnerFutureStartsNextWaitingTask },
        { "serialLaneShutdownRejectsWaitingTasks", serialLaneShutdownRejectsWaitingTasks },
    };
}
}
