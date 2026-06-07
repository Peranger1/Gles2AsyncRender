#include "execution_test_cases.h"

#include "framework/execution/execution.h"

#include <memory>
#include <vector>

namespace
{
using execution_test::require;
using execution_test::requireThrows;

// 本文件覆盖 TaskScheduler 与各类 Lane 组合后的真实调度链路。

void taskSchedulerFeedsSerialLaneWithoutRunningAhead()
{
    // 验证 ManualExecutor 未 drain 前，SerialLane 不会提前启动后续任务。
    auto executor = std::make_shared<async::ManualExecutor>();
    execution::TaskScheduler scheduler(executor);
    std::vector<int> started;
    std::vector<int> ran;

    execution::SerialLane<int, int> lane([&](int value) {
        started.push_back(value);
        return scheduler.submit([&, value]() {
            ran.push_back(value);
            return value * 10;
        });
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);

    require(started.size() == 1 && started[0] == 1,
            "SerialLane should only submit active work to TaskScheduler");
    require(executor->queuedCount() == 1,
            "TaskScheduler should hold active lane work until the manual executor drains");
    require(!first.isReady() && !second.isReady(),
            "SerialLane futures should wait for scheduled work");

    require(executor->drainOne(), "ManualExecutor should run the active scheduled task");
    require(first.get() == 10, "SerialLane should complete the first scheduled task");
    require(started.size() == 2 && started[1] == 2,
            "SerialLane should schedule the next task after the first completes");
    require(ran.size() == 1 && ran[0] == 1,
            "TaskScheduler should run only the drained task");
    require(!second.isReady(), "SerialLane should leave the second task pending until drained");

    require(executor->drainOne(), "ManualExecutor should run the second scheduled task");
    require(second.get() == 20, "SerialLane should complete the second scheduled task");
    require(ran.size() == 2 && ran[1] == 2,
            "TaskScheduler should run serial lane tasks in order");
    require(executor->empty(), "ManualExecutor should have no leftover scheduled work");
}

void taskSchedulerFeedsLatestLaneWithSupersededWaitingWork()
{
    // 验证 LatestLane 被 TaskScheduler 驱动时，superseded waiting 不会进入 executor。
    auto executor = std::make_shared<async::ManualExecutor>();
    execution::TaskScheduler scheduler(executor);
    std::vector<int> started;
    std::vector<int> ran;

    execution::LatestLane<int, int> lane([&](int value) {
        started.push_back(value);
        return scheduler.submit([&, value]() {
            ran.push_back(value);
            return value * 10;
        });
    });

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    requireThrows<execution::TaskSuperseded>([&]() {
        second.get();
    }, "LatestLane should reject superseded waiting work before scheduled execution");
    require(started.size() == 1 && started[0] == 1,
            "LatestLane should only schedule active work while the executor is pending");
    require(executor->queuedCount() == 1,
            "TaskScheduler should only queue the active latest-lane task");

    require(executor->drainOne(), "ManualExecutor should run the active latest-lane task");
    require(first.get() == 10, "LatestLane should complete active scheduled work");
    require(started.size() == 2 && started[1] == 3,
            "LatestLane should schedule the newest waiting work after active completion");
    require(ran.size() == 1 && ran[0] == 1,
            "TaskScheduler should not run superseded waiting work");

    require(executor->drainOne(), "ManualExecutor should run the latest waiting task");
    require(third.get() == 30, "LatestLane should complete newest scheduled work");
    require(ran.size() == 2 && ran[1] == 3,
            "TaskScheduler should run the active and latest tasks only");
    require(executor->empty(), "ManualExecutor should have no leftover latest-lane work");
}

struct SumMerger final
{
    bool canMerge(const int &, const int &) const
    {
        return true;
    }

    int merge(const int &oldValue, int incoming) const
    {
        return oldValue + incoming;
    }
};

void taskSchedulerFeedsMergeLaneWithMergedWaitingWork()
{
    // 验证 MergeLane 被 TaskScheduler 驱动时，只运行 active 和合并后的 waiting。
    auto executor = std::make_shared<async::ManualExecutor>();
    execution::TaskScheduler scheduler(executor);
    std::vector<int> started;
    std::vector<int> ran;

    execution::MergeLane<int, int, SumMerger> lane([&](int value) {
        started.push_back(value);
        return scheduler.submit([&, value]() {
            ran.push_back(value);
            return value * 10;
        });
    }, SumMerger());

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    requireThrows<execution::TaskSuperseded>([&]() {
        second.get();
    }, "MergeLane should supersede waiting work that was merged");
    require(started.size() == 1 && started[0] == 1,
            "MergeLane should only schedule active work while the executor is pending");
    require(executor->queuedCount() == 1,
            "TaskScheduler should only queue the active merge-lane task");

    require(executor->drainOne(), "ManualExecutor should run the active merge-lane task");
    require(first.get() == 10, "MergeLane should complete active scheduled work");
    require(started.size() == 2 && started[1] == 5,
            "MergeLane should schedule merged waiting args after active completion");
    require(ran.size() == 1 && ran[0] == 1,
            "TaskScheduler should not run superseded merge-lane work");

    require(executor->drainOne(), "ManualExecutor should run the merged waiting task");
    require(third.get() == 50, "MergeLane should complete merged scheduled work");
    require(ran.size() == 2 && ran[1] == 5,
            "TaskScheduler should run active and merged tasks only");
    require(executor->empty(), "ManualExecutor should have no leftover merge-lane work");
}
}

namespace execution_component_test
{
std::vector<execution_test::TestCase> executionIntegrationTestCases()
{
    return {
        { "taskSchedulerFeedsSerialLaneWithoutRunningAhead",
          taskSchedulerFeedsSerialLaneWithoutRunningAhead },
        { "taskSchedulerFeedsLatestLaneWithSupersededWaitingWork",
          taskSchedulerFeedsLatestLaneWithSupersededWaitingWork },
        { "taskSchedulerFeedsMergeLaneWithMergedWaitingWork",
          taskSchedulerFeedsMergeLaneWithMergedWaitingWork },
    };
}
}
