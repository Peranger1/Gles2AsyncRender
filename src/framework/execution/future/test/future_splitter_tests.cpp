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
}

namespace async_future_test
{
std::vector<execution_test::TestCase> splitterTestCases()
{
    return {
        { "futureSplitterFanoutBeforeCompletion", futureSplitterFanoutBeforeCompletion },
        { "futureSplitterReturnsReadyFutureAfterCompletion", futureSplitterReturnsReadyFutureAfterCompletion },
        { "futureSplitterBroadcastsException", futureSplitterBroadcastsException },
        { "futureSplitterRejectsInvalidSource", futureSplitterRejectsInvalidSource },
        { "futureSplitterSupportsUnit", futureSplitterSupportsUnit },
    };
}
}
