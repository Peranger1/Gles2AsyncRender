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

void mergeLaneRunnerThrowClearsActiveState()
{
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::MergeLane<int, int, SumMerger> lane([&](int value) {
        started.push_back(value);
        if (value == 2) {
            throw std::runtime_error("merge runner threw");
        }

        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    }, SumMerger());

    auto first = lane.submit(1);
    auto second = lane.submit(2);

    completions[0]->setValue(10);
    require(first.get() == 10, "MergeLane should complete active task before runner throw");
    requireThrows<std::runtime_error>([&]() {
        second.get();
    }, "MergeLane should fail the throwing runner task");

    auto third = lane.submit(3);
    require(started.size() == 3 && started[0] == 1 && started[1] == 2 && started[2] == 3,
            "MergeLane should clear active state after runner throw");

    completions[1]->setValue(30);
    require(third.get() == 30, "MergeLane should accept new work after runner throw");
}

void mergeLaneInvalidRunnerFutureClearsActiveState()
{
    std::vector<int> started;
    std::vector<std::shared_ptr<async::Promise<int>>> completions;

    execution::MergeLane<int, int, SumMerger> lane([&](int value) {
        started.push_back(value);
        if (value == 2) {
            return async::Future<int>();
        }

        auto promise = std::make_shared<async::Promise<int>>();
        auto future = promise->getFuture();
        completions.push_back(promise);
        return future;
    }, SumMerger());

    auto first = lane.submit(1);
    auto second = lane.submit(2);

    completions[0]->setValue(10);
    require(first.get() == 10, "MergeLane should complete active task before invalid runner future");
    requireThrows<async::FutureInvalid>([&]() {
        second.get();
    }, "MergeLane should fail an invalid runner future");

    auto third = lane.submit(3);
    require(started.size() == 3 && started[0] == 1 && started[1] == 2 && started[2] == 3,
            "MergeLane should clear active state after invalid runner future");

    completions[1]->setValue(30);
    require(third.get() == 30, "MergeLane should accept new work after invalid runner future");
}
}

namespace execution_component_test
{
std::vector<execution_test::TestCase> mergeLaneTestCases()
{
    return {
        { "mergeLaneMergesWaitingTask", mergeLaneMergesWaitingTask },
        { "mergeLaneRejectsUnmergeableIncomingTask", mergeLaneRejectsUnmergeableIncomingTask },
        { "mergeLaneRunnerThrowClearsActiveState", mergeLaneRunnerThrowClearsActiveState },
        { "mergeLaneInvalidRunnerFutureClearsActiveState", mergeLaneInvalidRunnerFutureClearsActiveState },
    };
}
}
