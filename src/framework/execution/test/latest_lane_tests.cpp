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

// 本文件覆盖 LatestLane 的 waiting 替换、stale 策略和 active 状态恢复。

void latestLaneSupersedesWaitingTask()
{
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::LatestLane<int, int> lane([&](int value) {
        started.push_back(value);
        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    requireThrows<execution::TaskSuperseded>([&]() {
        second.get();
    }, "LatestLane should supersede the previous waiting task");

    completions[0]->setValue(10);
    require(first.get() == 10, "LatestLane should complete the active task normally");
    require(started.size() == 2 && started[1] == 3,
            "LatestLane should start the latest waiting task next");

    completions[1]->setValue(30);
    require(third.get() == 30, "LatestLane should complete the latest task");
}

void latestLaneCanMarkActiveResultStaleWhenNewerWaitingExists()
{
    // 验证 stale 策略下，active 完成时若已有更新 waiting，则 active 结果不再交付。
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::LatestLane<int,
                          int,
                          execution::LatestLaneDelivery::MarkActiveStaleWhenWaiting> lane([&](int value) {
        started.push_back(value);
        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);

    completions[0]->setValue(10);
    requireThrows<execution::TaskStale>([&]() {
        first.get();
    }, "LatestLane stale policy should mark active result stale when newer waiting task exists");

    require(started.size() == 2 && started[1] == 2,
            "LatestLane stale policy should still start the newer waiting task");

    completions[1]->setValue(20);
    require(second.get() == 20, "LatestLane stale policy should deliver latest task result");
}

void latestLaneRunnerThrowClearsActiveState()
{
    // 验证 runner 失败后 lane 能接受新的提交。
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::LatestLane<int, int> lane([&](int value) {
        started.push_back(value);
        if (value == 2) {
            throw std::runtime_error("latest runner threw");
        }

        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);

    completions[0]->setValue(10);
    require(first.get() == 10, "LatestLane should complete active task before runner throw");
    requireThrows<std::runtime_error>([&]() {
        second.get();
    }, "LatestLane should fail the throwing runner task");

    auto third = lane.submit(3);
    require(started.size() == 3 && started[0] == 1 && started[1] == 2 && started[2] == 3,
            "LatestLane should clear active state after runner throw");

    completions[1]->setValue(30);
    require(third.get() == 30, "LatestLane should accept new work after runner throw");
}

void latestLaneInvalidRunnerFutureClearsActiveState()
{
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::LatestLane<int, int> lane([&](int value) {
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

    completions[0]->setValue(10);
    require(first.get() == 10, "LatestLane should complete active task before invalid runner future");
    requireThrows<async::FutureInvalid>([&]() {
        second.get();
    }, "LatestLane should fail an invalid runner future");

    auto third = lane.submit(3);
    require(started.size() == 3 && started[0] == 1 && started[1] == 2 && started[2] == 3,
            "LatestLane should clear active state after invalid runner future");

    completions[1]->setValue(30);
    require(third.get() == 30, "LatestLane should accept new work after invalid runner future");
}
}

namespace execution_component_test
{
std::vector<execution_test::TestCase> latestLaneTestCases()
{
    return {
        { "latestLaneSupersedesWaitingTask", latestLaneSupersedesWaitingTask },
        { "latestLaneCanMarkActiveResultStaleWhenNewerWaitingExists", latestLaneCanMarkActiveResultStaleWhenNewerWaitingExists },
        { "latestLaneRunnerThrowClearsActiveState", latestLaneRunnerThrowClearsActiveState },
        { "latestLaneInvalidRunnerFutureClearsActiveState", latestLaneInvalidRunnerFutureClearsActiveState },
    };
}
}
