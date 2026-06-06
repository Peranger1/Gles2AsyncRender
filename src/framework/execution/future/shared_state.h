#pragma once

#include "executor.h"
#include "exceptions.h"
#include "try.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace async
{
template <typename T>
class SharedState final
{
public:
    using Callback = std::function<void(Try<T> &&)>;
    using InterruptHandler = std::function<void(std::exception_ptr)>;

    void markFutureRetrieved()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_futureRetrieved) {
            throw FutureAlreadyRetrieved();
        }
        m_futureRetrieved = true;
    }

    bool isReady() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ready;
    }

    void setExecutor(std::shared_ptr<Executor> executor)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_executor = std::move(executor);
    }

    std::shared_ptr<Executor> executor() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_executor;
    }

    void setInterruptHandler(InterruptHandler handler)
    {
        InterruptHandler handlerToRun;
        std::exception_ptr interruptToDeliver;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_interruptHandler = std::move(handler);
            if (m_ready || !m_interrupt || !m_interruptHandler) {
                return;
            }

            handlerToRun = m_interruptHandler;
            interruptToDeliver = m_interrupt;
        }

        invokeInterruptHandler(handlerToRun, interruptToDeliver);
    }

    void raise(std::exception_ptr interrupt)
    {
        if (!interrupt) {
            interrupt = std::make_exception_ptr(FutureCancelled());
        }

        InterruptHandler handlerToRun;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_ready) {
                return;
            }

            m_interrupt = interrupt;
            if (!m_interruptHandler) {
                return;
            }
            handlerToRun = m_interruptHandler;
        }

        invokeInterruptHandler(handlerToRun, interrupt);
    }

    void setResult(Try<T> result)
    {
        Callback callback;
        std::shared_ptr<Executor> executor;
        std::shared_ptr<Try<T>> resultForCallback;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_ready) {
                throw PromiseAlreadySatisfied();
            }

            m_result = std::move(result);
            m_ready = true;
            m_cv.notify_all();

            if (m_callback) {
                callback = std::move(m_callback);
                executor = m_executor;
                resultForCallback = std::make_shared<Try<T>>(std::move(*m_result));
                m_result.reset();
            }
        }

        if (callback) {
            detail::schedule(executor, [callback = std::move(callback), resultForCallback]() mutable {
                callback(std::move(*resultForCallback));
            });
        }
    }

    void setCallback(Callback callback)
    {
        std::shared_ptr<Executor> executor;
        std::shared_ptr<Try<T>> readyResult;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_callback) {
                throw FutureInvalid();
            }

            if (m_ready) {
                executor = m_executor;
                readyResult = std::make_shared<Try<T>>(std::move(*m_result));
                m_result.reset();
            } else {
                m_callback = std::move(callback);
                return;
            }
        }

        detail::schedule(executor, [callback = std::move(callback), readyResult]() mutable {
            callback(std::move(*readyResult));
        });
    }

    Try<T> waitAndTake()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() {
            return m_ready;
        });

        if (!m_result.has_value()) {
            throw FutureInvalid();
        }

        Try<T> result = std::move(*m_result);
        m_result.reset();
        return result;
    }

private:
    static void invokeInterruptHandler(const InterruptHandler &handler, const std::exception_ptr &interrupt) noexcept
    {
        try {
            if (handler) {
                handler(interrupt);
            }
        } catch (...) {
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_ready = false;
    bool m_futureRetrieved = false;
    std::optional<Try<T>> m_result;
    Callback m_callback;
    std::shared_ptr<Executor> m_executor;
    InterruptHandler m_interruptHandler;
    std::exception_ptr m_interrupt;
};
}
