#pragma once

#include "exceptions.h"
#include "shared_state.h"
#include "try.h"
#include "unit.h"

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace async
{
template <typename T>
class Future;

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
        state->setCallback([nextState, funcHolder](Try<T> &&result) mutable {
            detail::fulfillStateWith<Result>(nextState, [&]() -> RawResult {
                return (*funcHolder)(std::move(result));
            });
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
        state->setCallback([nextState, funcHolder](Try<T> &&result) mutable {
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
        state->setCallback([nextState, funcHolder](Try<T> &&result) mutable {
            if (!result.hasException()) {
                nextState->setResult(std::move(result));
                return;
            }

            detail::fulfillStateWith<T>(nextState, [&]() {
                return (*funcHolder)(result.exception());
            });
        });

        return nextFuture;
    }

private:
    template <typename>
    friend class Promise;
    template <typename>
    friend class Future;

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
            std::move(innerFuture).thenTry([state](Try<typename IsFuture<Result>::Inner> &&result) {
                state->setResult(std::move(result));
                return Unit();
            });
        } else {
            state->setResult(Try<T>::fromValue(func()));
        }
    } catch (...) {
        state->setResult(Try<T>::fromException(std::current_exception()));
    }
}
}
}
