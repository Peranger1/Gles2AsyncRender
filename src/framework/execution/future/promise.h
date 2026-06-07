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
        // 析构负责兜底兑现未完成的状态，避免消费端永久等待。
        detach();
    }

    Promise(Promise &&other) noexcept
        : m_state(std::move(other.m_state))
        , m_abandonOnDestruct(other.m_abandonOnDestruct)
    {
        // 移动后由新 Promise 继承“等待内层 future 完成”的责任转移标志。
        other.m_abandonOnDestruct = false;
    }

    Promise &operator=(Promise &&other) noexcept
    {
        if (this != &other) {
            detach();
            m_state = std::move(other.m_state);
            m_abandonOnDestruct = other.m_abandonOnDestruct;
            other.m_abandonOnDestruct = false;
        }
        return *this;
    }

    Promise(const Promise &) = delete;
    Promise &operator=(const Promise &) = delete;

    // 每个 Promise 只能交出一个 Future，保证共享状态是单消费者模型。
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
        // exception_ptr 为空时 Try 会转成 FutureException，避免保存空异常。
        setTry(Try<T>::fromException(std::move(exception)));
    }

    void setTry(Try<T> result)
    {
        if (!m_state) {
            throw PromiseInvalid();
        }
        // setTry 是所有完成路径的公共出口，会触发 SharedState 中已注册的回调。
        m_state->setResult(std::move(result));
    }

    template <typename F>
    void setInterruptHandler(F &&func)
    {
        if (!m_state) {
            throw PromiseInvalid();
        }
        // 中断处理器只接收通知，不会自动改变 future 结果。
        m_state->setInterruptHandler(typename SharedState<T>::InterruptHandler(std::forward<F>(func)));
    }

    template <typename F>
    void setWith(F &&func)
    {
        if (!m_state) {
            throw PromiseInvalid();
        }

        std::shared_ptr<SharedState<T>> state = m_state;
        detail::fulfillStateWith<T>(state, std::forward<F>(func));

        using Result = std::invoke_result_t<F>;
        if constexpr (detail::IsFuture<Result>::value) {
            if (!state->isReady()) {
                // 外层完成责任已经转交给尚未完成的内层 future，析构时不能再写 BrokenPromise。
                m_abandonOnDestruct = true;
            }
        }
    }

    bool isFulfilled() const
    {
        // 被移动后的 Promise 视为已完成，方便调用方做防御性检查。
        return !m_state || m_state->isReady();
    }

private:
    void detach() noexcept
    {
        if (!m_state) {
            return;
        }

        try {
            // 生产端提前销毁时用 BrokenPromise 结束等待，避免消费端永久阻塞。
            if (!m_abandonOnDestruct && !m_state->isReady()) {
                m_state->setResult(Try<T>::fromException(std::make_exception_ptr(BrokenPromise())));
            }
        } catch (...) {
        }
        m_state.reset();
        m_abandonOnDestruct = false;
    }

    std::shared_ptr<SharedState<T>> m_state;
    bool m_abandonOnDestruct = false;
};
}
