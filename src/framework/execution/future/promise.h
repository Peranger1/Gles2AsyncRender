#pragma once

#include "exceptions.h"
#include "future.h"
#include "shared_state.h"
#include "unit.h"

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace async
{
template <typename T>
class Promise
{
public:
    Promise()
        : m_state(std::make_shared<SharedState<T>>())
    {
    }

    ~Promise()
    {
        detach();
    }

    Promise(Promise &&other) noexcept
        : m_state(std::move(other.m_state))
    {
    }

    Promise &operator=(Promise &&other) noexcept
    {
        if (this != &other) {
            detach();
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    Promise(const Promise &) = delete;
    Promise &operator=(const Promise &) = delete;

    Future<T> getFuture()
    {
        if (!m_state) {
            throw PromiseInvalid();
        }
        m_state->markFutureRetrieved();
        return Future<T>(m_state);
    }

    template <typename U = T>
    typename std::enable_if<!std::is_same<U, Unit>::value, void>::type setValue(U value)
    {
        setTry(Try<T>::fromValue(std::move(value)));
    }

    template <typename U = T>
    typename std::enable_if<std::is_same<U, Unit>::value, void>::type setValue()
    {
        setTry(Try<T>::fromValue(Unit()));
    }

    void setException(std::exception_ptr exception)
    {
        setTry(Try<T>::fromException(std::move(exception)));
    }

    void setTry(Try<T> result)
    {
        if (!m_state) {
            throw PromiseInvalid();
        }
        m_state->setResult(std::move(result));
    }

    template <typename F>
    void setInterruptHandler(F &&func)
    {
        if (!m_state) {
            throw PromiseInvalid();
        }
        m_state->setInterruptHandler(typename SharedState<T>::InterruptHandler(std::forward<F>(func)));
    }

    template <typename F>
    void setWith(F &&func)
    {
        try {
            using Result = std::invoke_result_t<F>;
            if constexpr (std::is_void<Result>::value) {
                static_assert(std::is_same<T, Unit>::value,
                              "A void producer can only fulfill Promise<Unit>.");
                func();
                setValue();
            } else {
                setValue(func());
            }
        } catch (...) {
            setException(std::current_exception());
        }
    }

    bool isFulfilled() const
    {
        return !m_state || m_state->isReady();
    }

private:
    void detach() noexcept
    {
        if (!m_state) {
            return;
        }

        try {
            if (!m_state->isReady()) {
                m_state->setResult(Try<T>::fromException(std::make_exception_ptr(BrokenPromise())));
            }
        } catch (...) {
        }
        m_state.reset();
    }

    std::shared_ptr<SharedState<T>> m_state;
};
}
