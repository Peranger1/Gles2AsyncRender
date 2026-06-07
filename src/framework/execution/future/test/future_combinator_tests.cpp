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

void tryValueRethrowsStoredException()
{
    async::Try<int> result = async::Try<int>::fromException(
        std::make_exception_ptr(std::runtime_error("stored")));

    require(result.hasException(), "Try should report stored exception");
    requireThrows<std::runtime_error>([&]() {
        result.value();
    }, "Try::value should rethrow the stored exception");
}

void makeReadyFutureProducesValue()
{
    auto future = async::makeReadyFuture(42);
    require(future.isReady(), "makeReadyFuture should create a ready future");
    require(future.get() == 42, "makeReadyFuture should store the provided value");
}

void makeReadyFutureProducesUnit()
{
    auto future = async::makeReadyFuture();
    require(future.isReady(), "makeReadyFuture() should create a ready Unit future");
    async::Unit unit = future.get();
    (void)unit;
}

void makeExceptionFuturePropagatesException()
{
    auto future = async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("ready error")));

    require(future.isReady(), "makeExceptionFuture should create a ready future");
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "makeExceptionFuture should rethrow its stored exception");
}

void collectAllEmptyReturnsReadyEmptyVector()
{
    std::vector<async::Future<int>> futures;
    auto collected = async::collectAll(std::move(futures));

    require(collected.isReady(), "collectAll of an empty vector should be ready");
    require(collected.get().empty(), "collectAll of an empty vector should return an empty result vector");
}

void collectAllPreservesValuesAndExceptions()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(10));
    futures.push_back(async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("middle"))));
    futures.push_back(async::makeReadyFuture(32));

    auto collected = async::collectAll(std::move(futures));
    std::vector<async::Try<int>> results = collected.get();

    require(results.size() == 3, "collectAll should preserve result count");
    require(results[0].hasValue(), "collectAll result 0 should contain a value");
    require(results[0].value() == 10, "collectAll result 0 should preserve its value");
    require(results[1].hasException(), "collectAll result 1 should contain an exception");
    require(results[2].hasValue(), "collectAll result 2 should contain a value");
    require(results[2].value() == 32, "collectAll result 2 should preserve its value");

    requireThrows<std::runtime_error>([&]() {
        results[1].value();
    }, "collectAll should keep exceptions in their Try slot");
}

void collectAllWaitsForAsynchronousFutures()
{
    auto executor = std::make_shared<async::ThreadPoolExecutor>(2);
    std::vector<async::Future<int>> futures;
    futures.push_back(async::async(executor, []() {
        return 20;
    }));
    futures.push_back(async::async(executor, []() {
        return 22;
    }));

    auto collected = async::collectAll(std::move(futures));
    std::vector<async::Try<int>> results = collected.get();

    require(results.size() == 2, "collectAll should collect asynchronous futures");
    require(results[0].value() + results[1].value() == 42,
            "collectAll should preserve asynchronous values");
}

void collectAllRejectsInvalidFuture()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::Future<int>());

    auto collected = async::collectAll(std::move(futures));
    require(collected.isReady(), "collectAll should reject invalid futures immediately");
    requireThrows<async::FutureInvalid>([&]() {
        collected.get();
    }, "collectAll should complete with FutureInvalid when an input future is invalid");
}

void collectReturnsValuesWhenAllSucceed()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(10));
    futures.push_back(async::makeReadyFuture(32));

    auto collected = async::collect(std::move(futures));
    std::vector<int> values = collected.get();

    require(values.size() == 2, "collect should preserve value count");
    require(values[0] == 10 && values[1] == 32,
            "collect should preserve successful values in order");
}

void collectPropagatesFirstStoredException()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(1));
    futures.push_back(async::makeExceptionFuture<int>(
        std::make_exception_ptr(std::runtime_error("collect failed"))));
    futures.push_back(async::makeReadyFuture(3));

    auto collected = async::collect(std::move(futures));
    requireThrows<std::runtime_error>([&]() {
        collected.get();
    }, "collect should fail when any input future fails");
}

void collectEmptyReturnsReadyEmptyVector()
{
    std::vector<async::Future<int>> futures;
    auto collected = async::collect(std::move(futures));

    require(collected.isReady(), "collect of an empty vector should be ready");
    require(collected.get().empty(), "collect of an empty vector should return an empty vector");
}

void collectAnyReturnsFirstSuccessfulCompletion()
{
    async::Promise<int> firstPromise;
    async::Promise<int> secondPromise;

    std::vector<async::Future<int>> futures;
    futures.push_back(firstPromise.getFuture());
    futures.push_back(secondPromise.getFuture());

    auto firstCompleted = async::collectAny(std::move(futures));
    secondPromise.setValue(42);

    auto result = firstCompleted.get();
    require(result.first == 1, "collectAny should report the first completed index");
    require(result.second.hasValue(), "collectAny should preserve a successful first result");
    require(result.second.value() == 42, "collectAny should preserve the first completed value");

    firstPromise.setValue(7);
}

void collectAnyReturnsFirstFailedCompletion()
{
    async::Promise<int> firstPromise;
    async::Promise<int> secondPromise;

    std::vector<async::Future<int>> futures;
    futures.push_back(firstPromise.getFuture());
    futures.push_back(secondPromise.getFuture());

    auto firstCompleted = async::collectAny(std::move(futures));
    firstPromise.setException(std::make_exception_ptr(std::runtime_error("first failed")));

    auto result = firstCompleted.get();
    require(result.first == 0, "collectAny should report a failed first completion index");
    require(result.second.hasException(), "collectAny should preserve a failed first result");
    requireThrows<std::runtime_error>([&]() {
        result.second.value();
    }, "collectAny should keep the first completion exception");

    secondPromise.setValue(42);
}

void collectAnyEmptyReturnsException()
{
    std::vector<async::Future<int>> futures;
    auto firstCompleted = async::collectAny(std::move(futures));

    require(firstCompleted.isReady(), "collectAny of an empty vector should be ready with an exception");
    requireThrows<async::FutureInvalid>([&]() {
        firstCompleted.get();
    }, "collectAny of an empty vector should fail");
}

void collectAnyRejectsInvalidFuture()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(1));
    futures.push_back(async::Future<int>());

    auto firstCompleted = async::collectAny(std::move(futures));
    require(firstCompleted.isReady(), "collectAny should reject invalid futures immediately");
    requireThrows<async::FutureInvalid>([&]() {
        firstCompleted.get();
    }, "collectAny should complete with FutureInvalid when an input future is invalid");
}

void collectAnyUsesFirstReadyFutureInInputOrder()
{
    std::vector<async::Future<int>> futures;
    futures.push_back(async::makeReadyFuture(10));
    futures.push_back(async::makeReadyFuture(32));

    auto firstCompleted = async::collectAny(std::move(futures));
    auto result = firstCompleted.get();

    require(result.first == 0, "collectAny should use the first ready future in input order");
    require(result.second.value() == 10, "collectAny should preserve the first ready value");
}
}

namespace async_future_test
{
std::vector<execution_test::TestCase> combinatorTestCases()
{
    return {
        { "tryValueRethrowsStoredException", tryValueRethrowsStoredException },
        { "makeReadyFutureProducesValue", makeReadyFutureProducesValue },
        { "makeReadyFutureProducesUnit", makeReadyFutureProducesUnit },
        { "makeExceptionFuturePropagatesException", makeExceptionFuturePropagatesException },
        { "collectAllEmptyReturnsReadyEmptyVector", collectAllEmptyReturnsReadyEmptyVector },
        { "collectAllPreservesValuesAndExceptions", collectAllPreservesValuesAndExceptions },
        { "collectAllWaitsForAsynchronousFutures", collectAllWaitsForAsynchronousFutures },
        { "collectAllRejectsInvalidFuture", collectAllRejectsInvalidFuture },
        { "collectReturnsValuesWhenAllSucceed", collectReturnsValuesWhenAllSucceed },
        { "collectPropagatesFirstStoredException", collectPropagatesFirstStoredException },
        { "collectEmptyReturnsReadyEmptyVector", collectEmptyReturnsReadyEmptyVector },
        { "collectAnyReturnsFirstSuccessfulCompletion", collectAnyReturnsFirstSuccessfulCompletion },
        { "collectAnyReturnsFirstFailedCompletion", collectAnyReturnsFirstFailedCompletion },
        { "collectAnyEmptyReturnsException", collectAnyEmptyReturnsException },
        { "collectAnyRejectsInvalidFuture", collectAnyRejectsInvalidFuture },
        { "collectAnyUsesFirstReadyFutureInInputOrder", collectAnyUsesFirstReadyFutureInInputOrder },
    };
}
}
