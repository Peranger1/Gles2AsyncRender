#include "framework/execution/execution.h"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

void taskSchedulerRunsSubmittedTask()
{
    execution::TaskScheduler scheduler(async::InlineExecutor::instance());

    auto future = scheduler.submit([]() {
        return 42;
    });

    require(future.get() == 42, "TaskScheduler should complete with submitted task value");
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

void mergeLaneMergesWaitingTask()
{
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::MergeLane<int, int, SumMerger> lane([&](int value) {
        started.push_back(value);
        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    }, SumMerger());

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    requireThrows<execution::TaskSuperseded>([&]() {
        second.get();
    }, "MergeLane should supersede the old waiting task when merging");

    completions[0]->setValue(10);
    require(first.get() == 10, "MergeLane should complete the active task normally");
    require(started.size() == 2 && started[1] == 5,
            "MergeLane should start the merged waiting args");

    completions[1]->setValue(50);
    require(third.get() == 50, "MergeLane should complete the merged task future");
}

struct RejectMerge final
{
    bool canMerge(const int &, const int &) const
    {
        return false;
    }

    int merge(const int &, int incoming) const
    {
        return incoming;
    }
};

void mergeLaneRejectsUnmergeableIncomingTask()
{
    auto activePromise = std::make_shared<async::Promise<int>>();
    execution::MergeLane<int, int, RejectMerge> lane([&](int value) {
        if (value == 1) {
            return activePromise->getFuture();
        }
        return async::makeReadyFuture(value);
    }, RejectMerge());

    auto first = lane.submit(1);
    auto second = lane.submit(2);
    auto third = lane.submit(3);

    requireThrows<execution::TaskRejected>([&]() {
        third.get();
    }, "MergeLane should reject incoming tasks that cannot merge with waiting");

    activePromise->setValue(10);
    require(first.get() == 10, "MergeLane should complete active task normally");
    require(second.get() == 2, "MergeLane should still run the existing waiting task");
}

using TestCase = std::pair<const char *, std::function<void()>>;

std::vector<TestCase> testCases()
{
    return {
        { "taskSchedulerRunsSubmittedTask", taskSchedulerRunsSubmittedTask },
        { "taskSchedulerCapturesThrownException", taskSchedulerCapturesThrownException },
        { "taskSchedulerRejectsAfterShutdown", taskSchedulerRejectsAfterShutdown },
        { "taskSchedulerRejectsMissingExecutor", taskSchedulerRejectsMissingExecutor },
        { "taskSchedulerFlattensReturnedFuture", taskSchedulerFlattensReturnedFuture },
        { "serialLaneRunsOneTaskAtATimeInOrder", serialLaneRunsOneTaskAtATimeInOrder },
        { "serialLanePropagatesRunnerFailure", serialLanePropagatesRunnerFailure },
        { "serialLaneShutdownRejectsWaitingTasks", serialLaneShutdownRejectsWaitingTasks },
        { "latestLaneSupersedesWaitingTask", latestLaneSupersedesWaitingTask },
        { "latestLaneCanMarkActiveResultStaleWhenNewerWaitingExists", latestLaneCanMarkActiveResultStaleWhenNewerWaitingExists },
        { "mergeLaneMergesWaitingTask", mergeLaneMergesWaitingTask },
        { "mergeLaneRejectsUnmergeableIncomingTask", mergeLaneRejectsUnmergeableIncomingTask },
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
        std::cerr << failedCount << " execution test(s) failed.\n";
        return 1;
    }

    std::cout << "All execution tests passed.\n";
    return 0;
}
