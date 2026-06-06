#pragma once

#include "exceptions.h"

#include <exception>
#include <optional>
#include <utility>

namespace async
{
template <typename T>
class Try
{
public:
    Try() = default;

    explicit Try(T value)
        : m_value(std::move(value))
    {
    }

    explicit Try(std::exception_ptr exception)
        : m_exception(std::move(exception))
    {
        if (!m_exception) {
            m_exception = std::make_exception_ptr(FutureException("Empty exception"));
        }
    }

    static Try<T> fromValue(T value)
    {
        return Try<T>(std::move(value));
    }

    static Try<T> fromException(std::exception_ptr exception)
    {
        return Try<T>(std::move(exception));
    }

    bool hasValue() const
    {
        return m_value.has_value();
    }

    bool hasException() const
    {
        return m_exception != nullptr;
    }

    T &value() &
    {
        throwIfNoValue();
        return *m_value;
    }

    const T &value() const &
    {
        throwIfNoValue();
        return *m_value;
    }

    T &&value() &&
    {
        throwIfNoValue();
        return std::move(*m_value);
    }

    std::exception_ptr exception() const
    {
        return m_exception;
    }

private:
    void throwIfNoValue() const
    {
        if (m_exception) {
            std::rethrow_exception(m_exception);
        }
        if (!m_value.has_value()) {
            throw FutureInvalid();
        }
    }

    std::optional<T> m_value;
    std::exception_ptr m_exception;
};
}
