#pragma once

#include "exceptions.h"
#include "shared_state.h"
#include "try.h"
#include "unit.h"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace async
{
template <typename T>
class Future;

class InterruptHandle final
{
public:
    InterruptHandle() = default;

    explicit InterruptHandle(std::function<void(std::exception_ptr)> raiseFunc)
        : m_raiseFunc(std::move(raiseFunc))
    {
    }

    bool valid() const
    {
        return bool(m_raiseFunc);
    }

    void raise(std::exception_ptr interrupt) const
    {
        if (m_raiseFunc) {
            m_raiseFunc(std::move(interrupt));
        }
    }

private:
    std::function<void(std::exception_ptr)> m_raiseFunc;
};

namespace detail
{
template <typename T>
struct IsFuture
{
    static constexpr bool value = false;
};

template <typename T>
struct IsFuture<Future<T>>
{
    static constexpr bool value = true;
    using Inner = T;
};

template <typename T>
struct FutureValue
{
    using Type = T;
};

template <typename T>
struct FutureValue<Future<T>>
{
    using Type = T;
};

template <>
struct FutureValue<void>
{
    using Type = Unit;
};

template <typename T, typename F>
struct ThenValueResult
{
    using Type = std::invoke_result_t<F, T &&>;
};

template <typename F>
struct ThenValueResult<Unit, F>
{
    using Type = std::invoke_result_t<F>;
};

template <typename T, typename F>
void fulfillStateWith(const std::shared_ptr<SharedState<T>> &state, F &&func);

template <typename T>
void failStateWithExecutorRejected(const std::shared_ptr<SharedState<T>> &state) noexcept;
}

template <typename T>
class Future
{
public:
    Future() = default;

    Future(Future &&) noexcept = default;
    Future &operator=(Future &&) noexcept = default;

    Future(const Future &) = delete;
    Future &operator=(const Future &) = delete;

    bool valid() const
    {
        return m_state != nullptr;
    }

    bool isReady() const
    {
        return m_state && m_state->isReady();
    }

    InterruptHandle interruptHandle() const
    {
        throwIfInvalid();
        std::shared_ptr<SharedState<T>> state = m_state;
        return InterruptHandle([state](std::exception_ptr interrupt) {
            state->raise(std::move(interrupt));
        });
    }

    void cancel()
    {
        raise(std::make_exception_ptr(FutureCancelled()));
    }

    void raise(std::exception_ptr interrupt)
    {
        throwIfInvalid();
        m_state->raise(std::move(interrupt));
    }

    Future<T> via(std::shared_ptr<Executor> executor) &&
    {
        throwIfInvalid();
        m_state->setExecutor(std::move(executor));
        return std::move(*this);
    }

    T get()
    {
        throwIfInvalid();
        std::shared_ptr<SharedState<T>> state = std::move(m_state);
        Try<T> result = state->waitAndTake();
        return std::move(result).value();
    }

    template <typename Rep, typename Period>
    bool waitFor(std::chrono::duration<Rep, Period> timeout) const
    {
        throwIfInvalid();
        return m_state->waitFor(timeout);
    }

    template <typename Clock, typename Duration>
    bool waitUntil(std::chrono::time_point<Clock, Duration> deadline) const
    {
        throwIfInvalid();
        return m_state->waitUntil(deadline);
    }

    template <typename Rep, typename Period>
    T getFor(std::chrono::duration<Rep, Period> timeout)
    {
        throwIfInvalid();
        std::shared_ptr<SharedState<T>> state = m_state;
        if (!state->waitFor(timeout)) {
            throw FutureTimeout();
        }

        m_state.reset();
        Try<T> result = state->takeResult();
        return std::move(result).value();
    }

    template <typename Clock, typename Duration>
    T getUntil(std::chrono::time_point<Clock, Duration> deadline)
    {
        throwIfInvalid();
        std::shared_ptr<SharedState<T>> state = m_state;
        if (!state->waitUntil(deadline)) {
            throw FutureTimeout();
        }

        m_state.reset();
        Try<T> result = state->takeResult();
        return std::move(result).value();
    }

    template <typename F>
    auto thenTry(F &&func)
        -> Future<typename detail::FutureValue<std::invoke_result_t<F, Try<T> &&>>::Type>
    {
        throwIfInvalid();

        using RawResult = std::invoke_result_t<F, Try<T> &&>;
        using Result = typename detail::FutureValue<RawResult>::Type;
        auto nextState = std::make_shared<SharedState<Result>>();
        Future<Result> nextFuture(nextState);
        auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));

        std::shared_ptr<SharedState<T>> state = std::move(m_state);
        state->setCallback(
            [nextState, funcHolder](Try<T> &&result) mutable {
                detail::fulfillStateWith<Result>(nextState, [&]() -> RawResult {
                    return (*funcHolder)(std::move(result));
                });
            },
            [nextState]() {
                detail::failStateWithExecutorRejected(nextState);
            });

        return nextFuture;
    }

    template <typename F>
    auto thenValue(F &&func)
        -> Future<typename detail::FutureValue<typename detail::ThenValueResult<T, F>::Type>::Type>
    {
        throwIfInvalid();

        using RawResult = typename detail::ThenValueResult<T, F>::Type;
        using Result = typename detail::FutureValue<RawResult>::Type;
        auto nextState = std::make_shared<SharedState<Result>>();
        Future<Result> nextFuture(nextState);
        auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));

        std::shared_ptr<SharedState<T>> state = std::move(m_state);
        state->setCallback(
            [nextState, funcHolder](Try<T> &&result) mutable {
                if (result.hasException()) {
                    nextState->setResult(Try<Result>::fromException(result.exception()));
                    return;
                }

                detail::fulfillStateWith<Result>(nextState, [&]() -> RawResult {
                    if constexpr (std::is_same<T, Unit>::value) {
                        (void)result;
                        return (*funcHolder)();
                    } else {
                        return (*funcHolder)(std::move(result).value());
                    }
                });
            },
            [nextState]() {
                detail::failStateWithExecutorRejected(nextState);
            });

        return nextFuture;
    }

    template <typename F>
    Future<T> thenError(F &&func)
    {
        throwIfInvalid();

        auto nextState = std::make_shared<SharedState<T>>();
        Future<T> nextFuture(nextState);
        auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));

        std::shared_ptr<SharedState<T>> state = std::move(m_state);
        state->setCallback(
            [nextState, funcHolder](Try<T> &&result) mutable {
                if (!result.hasException()) {
                    nextState->setResult(std::move(result));
                    return;
                }

                detail::fulfillStateWith<T>(nextState, [&]() {
                    return (*funcHolder)(result.exception());
                });
            },
            [nextState]() {
                detail::failStateWithExecutorRejected(nextState);
            });

        return nextFuture;
    }

    template <typename E, typename F>
    Future<T> thenError(F &&func)
    {
        throwIfInvalid();

        auto nextState = std::make_shared<SharedState<T>>();
        Future<T> nextFuture(nextState);
        auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));

        std::shared_ptr<SharedState<T>> state = std::move(m_state);
        state->setCallback(
            [nextState, funcHolder](Try<T> &&result) mutable {
                if (!result.hasException()) {
                    nextState->setResult(std::move(result));
                    return;
                }

                try {
                    std::rethrow_exception(result.exception());
                } catch (const E &exception) {
                    detail::fulfillStateWith<T>(nextState, [&]() {
                        return (*funcHolder)(exception);
                    });
                } catch (...) {
                    nextState->setResult(Try<T>::fromException(result.exception()));
                }
            },
            [nextState]() {
                detail::failStateWithExecutorRejected(nextState);
            });

        return nextFuture;
    }

    template <typename F>
    Future<T> ensure(F &&func)
    {
        throwIfInvalid();

        auto nextState = std::make_shared<SharedState<T>>();
        Future<T> nextFuture(nextState);
        auto funcHolder = std::make_shared<typename std::decay<F>::type>(std::forward<F>(func));

        std::shared_ptr<SharedState<T>> state = std::move(m_state);
        state->setCallback(
            [nextState, funcHolder](Try<T> &&result) mutable {
                try {
                    using Result = std::invoke_result_t<F>;
                    if constexpr (std::is_void<Result>::value) {
                        (*funcHolder)();
                    } else {
                        (void)(*funcHolder)();
                    }
                    nextState->setResult(std::move(result));
                } catch (...) {
                    nextState->setResult(Try<T>::fromException(std::current_exception()));
                }
            },
            [nextState]() {
                detail::failStateWithExecutorRejected(nextState);
            });

        return nextFuture;
    }

private:
    template <typename>
    friend class Promise;
    template <typename>
    friend class Future;
    template <typename U, typename F>
    friend void detail::fulfillStateWith(const std::shared_ptr<SharedState<U>> &state, F &&func);

    explicit Future(std::shared_ptr<SharedState<T>> state)
        : m_state(std::move(state))
    {
    }

    void throwIfInvalid() const
    {
        if (!m_state) {
            throw FutureInvalid();
        }
    }

    std::shared_ptr<SharedState<T>> m_state;
};

namespace detail
{
template <typename T, typename F>
void fulfillStateWith(const std::shared_ptr<SharedState<T>> &state, F &&func)
{
    try {
        using Result = std::invoke_result_t<F>;
        if constexpr (std::is_void<Result>::value) {
            static_assert(std::is_same<T, Unit>::value,
                          "A void continuation can only fulfill Future<Unit>.");
            func();
            state->setResult(Try<T>::fromValue(Unit()));
        } else if constexpr (IsFuture<Result>::value) {
            auto innerFuture = func();
            if (!innerFuture.valid()) {
                state->setResult(Try<T>::fromException(std::make_exception_ptr(FutureInvalid())));
                return;
            }

            std::shared_ptr<SharedState<typename IsFuture<Result>::Inner>> innerState =
                std::move(innerFuture.m_state);
            innerState->setCallback(
                [state](Try<typename IsFuture<Result>::Inner> &&result) {
                    state->setResult(std::move(result));
                },
                [state]() {
                    failStateWithExecutorRejected(state);
                });
        } else {
            state->setResult(Try<T>::fromValue(func()));
        }
    } catch (...) {
        state->setResult(Try<T>::fromException(std::current_exception()));
    }
}

template <typename T>
void failStateWithExecutorRejected(const std::shared_ptr<SharedState<T>> &state) noexcept
{
    try {
        state->setResult(Try<T>::fromException(std::make_exception_ptr(ExecutorRejected())));
    } catch (...) {
    }
}
}
}
