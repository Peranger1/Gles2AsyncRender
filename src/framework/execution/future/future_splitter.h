#pragma once

#include "combinators.h"
#include "future.h"
#include "promise.h"
#include "try.h"
#include "unit.h"

#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace async
{
namespace detail
{
template <typename T>
Try<T> copyTry(const Try<T> &result)
{
    if (result.hasException()) {
        return Try<T>::fromException(result.exception());
    }
    return Try<T>::fromValue(result.value());
}

template <>
inline Try<Unit> copyTry<Unit>(const Try<Unit> &result)
{
    if (result.hasException()) {
        return Try<Unit>::fromException(result.exception());
    }
    return Try<Unit>::fromValue(Unit());
}

template <typename T>
struct FutureSplitterState final
{
    std::mutex mutex;
    std::optional<Try<T>> result;
    std::vector<Promise<T>> waiters;
};
}

template <typename T>
class FutureSplitter final
{
public:
    FutureSplitter() = default;

    explicit FutureSplitter(Future<T> future)
        : m_state(std::make_shared<detail::FutureSplitterState<T>>())
    {
        static_assert(std::is_copy_constructible<T>::value,
                      "FutureSplitter<T> currently requires T to be copy constructible.");

        if (!future.valid()) {
            m_state->result = Try<T>::fromException(std::make_exception_ptr(FutureInvalid()));
            return;
        }

        auto state = m_state;
        std::move(future).thenTry([state](Try<T> &&result) {
            std::vector<Promise<T>> waiters;
            Try<T> stored = result.hasException()
                ? Try<T>::fromException(result.exception())
                : Try<T>::fromValue(std::move(result).value());
            Try<T> storedForWaiters = detail::copyTry(stored);

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(stored);
                waiters = std::move(state->waiters);
                state->waiters.clear();
            }

            for (Promise<T> &waiter : waiters) {
                waiter.setTry(detail::copyTry(storedForWaiters));
            }
            return Unit();
        });
    }

    FutureSplitter(FutureSplitter &&) noexcept = default;
    FutureSplitter &operator=(FutureSplitter &&) noexcept = default;

    FutureSplitter(const FutureSplitter &) = delete;
    FutureSplitter &operator=(const FutureSplitter &) = delete;

    bool valid() const
    {
        return m_state != nullptr;
    }

    Future<T> getFuture()
    {
        if (!m_state) {
            return makeExceptionFuture<T>(std::make_exception_ptr(FutureInvalid()));
        }

        Promise<T> promise;
        Future<T> future = promise.getFuture();

        std::optional<Try<T>> result;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->result.has_value()) {
                result = detail::copyTry(*m_state->result);
            } else {
                m_state->waiters.push_back(std::move(promise));
                return future;
            }
        }

        promise.setTry(std::move(*result));
        return future;
    }

private:
    std::shared_ptr<detail::FutureSplitterState<T>> m_state;
};
}
