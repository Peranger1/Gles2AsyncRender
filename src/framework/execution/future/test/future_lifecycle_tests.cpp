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

void promiseCanBeFulfilledBeforeFutureRetrieval()
{
    async::Promise<int> promise;
    promise.setValue(42);

    auto future = promise.getFuture();
    require(future.isReady(), "future should be ready after promise was fulfilled");
    require(future.get() == 42, "future should return the fulfilled value");
}

void promiseRejectsDuplicateFutureRetrieval()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    (void)future;

    requireThrows<async::FutureAlreadyRetrieved>([&]() {
        promise.getFuture();
    }, "promise should reject duplicate future retrieval");
}

void promiseRejectsDuplicateFulfillment()
{
    async::Promise<int> promise;
    promise.setValue(1);

    requireThrows<async::PromiseAlreadySatisfied>([&]() {
        promise.setValue(2);
    }, "promise should reject duplicate fulfillment");
}

void futureCannotBeUsedAfterGet()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    promise.setValue(42);

    require(future.get() == 42, "future should return the first result");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "future should be invalid after get");
}

void waitForTimeoutDoesNotConsumeFuture()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();

    require(!future.waitFor(std::chrono::milliseconds(1)),
            "waitFor should return false while the future is still pending");

    promise.setValue(42);
    require(future.get() == 42, "waitFor timeout should not consume the future");
}

void waitForReadyDoesNotConsumeFuture()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    promise.setValue(42);

    require(future.waitFor(std::chrono::milliseconds(1)),
            "waitFor should return true for a ready future");
    require(future.get() == 42, "waitFor ready should not consume the future");
}

void getForTimeoutDoesNotConsumeFuture()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();

    requireThrows<async::FutureTimeout>([&]() {
        future.getFor(std::chrono::milliseconds(1));
    }, "getFor should throw FutureTimeout while pending");

    promise.setValue(42);
    require(future.get() == 42, "getFor timeout should not consume the future");
}

void getForReadyConsumesFuture()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    promise.setValue(42);

    require(future.getFor(std::chrono::milliseconds(1)) == 42,
            "getFor should return a ready result");
    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "getFor success should consume the future");
}

void futureCannotBeUsedAfterThenValue()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    auto next = future.thenValue([](int value) {
        return value + 1;
    });

    requireThrows<async::FutureInvalid>([&]() {
        future.get();
    }, "future should be invalid after attaching a continuation");

    promise.setValue(41);
    require(next.get() == 42, "downstream future should still receive the result");
}

void brokenPromiseCompletesFutureWithException()
{
    async::Future<int> future;
    {
        async::Promise<int> promise;
        future = promise.getFuture();
    }

    requireThrows<async::BrokenPromise>([&]() {
        future.get();
    }, "destroying an unfulfilled promise should complete with BrokenPromise");
}

void cancelAfterHandlerNotifiesProducer()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool interrupted = false;

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        interrupted = true;
        try {
            std::rethrow_exception(reason);
        } catch (const async::FutureCancelled &) {
            promise.setValue(42);
        }
    });

    future.cancel();
    require(interrupted, "cancel should notify the registered interrupt handler");
    require(future.get() == 42, "interrupt handler should be able to fulfill the promise");
}

void cancelBeforeHandlerIsDeliveredWhenHandlerIsSet()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool interrupted = false;

    future.cancel();
    require(!future.isReady(), "cancel should not complete the future by itself");

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        interrupted = true;
        try {
            std::rethrow_exception(reason);
        } catch (const async::FutureCancelled &) {
            promise.setException(std::make_exception_ptr(std::runtime_error("cancelled")));
        }
    });

    require(interrupted, "stored cancel should be delivered when handler is registered");
    requireThrows<std::runtime_error>([&]() {
        future.get();
    }, "handler should be able to complete the future with its own exception");
}

void raiseDeliversCustomReason()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool receivedCustomReason = false;

    promise.setInterruptHandler([&](std::exception_ptr reason) {
        try {
            std::rethrow_exception(reason);
        } catch (const std::runtime_error &e) {
            receivedCustomReason = std::string(e.what()) == "stop";
        }
        promise.setValue(42);
    });

    future.raise(std::make_exception_ptr(std::runtime_error("stop")));
    require(receivedCustomReason, "raise should deliver the custom interrupt reason");
    require(future.get() == 42, "future should still be completed by the producer");
}

void cancelAfterFulfilledDoesNothing()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();
    bool interrupted = false;

    promise.setInterruptHandler([&](std::exception_ptr) {
        interrupted = true;
    });
    promise.setValue(42);

    future.cancel();
    require(!interrupted, "cancel after fulfillment should not notify the handler");
    require(future.get() == 42, "cancel after fulfillment should not change the result");
}

void interruptHandlerRunsOutsideSharedStateLock()
{
    async::Promise<int> promise;
    auto future = promise.getFuture();

    promise.setInterruptHandler([&](std::exception_ptr) {
        promise.setValue(42);
    });

    future.cancel();
    require(future.get() == 42, "interrupt handler should be able to complete without deadlock");
}
}

namespace async_future_test
{
std::vector<execution_test::TestCase> lifecycleTestCases()
{
    return {
        { "promiseCanBeFulfilledBeforeFutureRetrieval", promiseCanBeFulfilledBeforeFutureRetrieval },
        { "promiseRejectsDuplicateFutureRetrieval", promiseRejectsDuplicateFutureRetrieval },
        { "promiseRejectsDuplicateFulfillment", promiseRejectsDuplicateFulfillment },
        { "futureCannotBeUsedAfterGet", futureCannotBeUsedAfterGet },
        { "waitForTimeoutDoesNotConsumeFuture", waitForTimeoutDoesNotConsumeFuture },
        { "waitForReadyDoesNotConsumeFuture", waitForReadyDoesNotConsumeFuture },
        { "getForTimeoutDoesNotConsumeFuture", getForTimeoutDoesNotConsumeFuture },
        { "getForReadyConsumesFuture", getForReadyConsumesFuture },
        { "futureCannotBeUsedAfterThenValue", futureCannotBeUsedAfterThenValue },
        { "brokenPromiseCompletesFutureWithException", brokenPromiseCompletesFutureWithException },
        { "cancelAfterHandlerNotifiesProducer", cancelAfterHandlerNotifiesProducer },
        { "cancelBeforeHandlerIsDeliveredWhenHandlerIsSet", cancelBeforeHandlerIsDeliveredWhenHandlerIsSet },
        { "raiseDeliversCustomReason", raiseDeliversCustomReason },
        { "cancelAfterFulfilledDoesNothing", cancelAfterFulfilledDoesNothing },
        { "interruptHandlerRunsOutsideSharedStateLock", interruptHandlerRunsOutsideSharedStateLock },
    };
}
}
