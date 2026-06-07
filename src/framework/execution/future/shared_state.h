#pragma once

#include "executor.h"
#include "exceptions.h"
#include "try.h"

#include <condition_variable>
#include <chrono>
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
    using ScheduleFailure = std::function<void()>;
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

            // 已经收到过 interrupt 时，在注册 handler 后补投递一次。
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
                // handler 尚未注册时先保存 interrupt，后续 setInterruptHandler 会补投递。
                return;
            }
            handlerToRun = m_interruptHandler;
        }

        invokeInterruptHandler(handlerToRun, interrupt);
    }

    void setResult(Try<T> result)
    {
        Callback callback;
        ScheduleFailure scheduleFailure;
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
                scheduleFailure = std::move(m_scheduleFailure);
                executor = m_executor;
                resultForCallback = std::make_shared<Try<T>>(std::move(*m_result));
                m_result.reset();
            }
        }

        if (callback) {
            // 用户 callback 在锁外调度，避免 continuation 重入 shared state 锁。
            const bool scheduled = detail::schedule(executor, [callback = std::move(callback), resultForCallback]() mutable {
                callback(std::move(*resultForCallback));
            });
            if (!scheduled) {
                invokeScheduleFailure(scheduleFailure);
            }
        }
    }

    void setCallback(Callback callback, ScheduleFailure scheduleFailure = {})
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
                // 未完成时只保存 callback；真正执行发生在 setResult 的锁外阶段。
                m_callback = std::move(callback);
                m_scheduleFailure = std::move(scheduleFailure);
                return;
            }
        }

        // 已完成的 future 追加 callback 时，同样通过 executor 调度，不在锁内直接执行。
        const bool scheduled = detail::schedule(executor, [callback = std::move(callback), readyResult]() mutable {
            callback(std::move(*readyResult));
        });
        if (!scheduled) {
            invokeScheduleFailure(scheduleFailure);
        }
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() {
            return m_ready;
        });
    }

    template <typename Rep, typename Period>
    bool waitFor(std::chrono::duration<Rep, Period> timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this]() {
            return m_ready;
        });
    }

    template <typename Clock, typename Duration>
    bool waitUntil(std::chrono::time_point<Clock, Duration> deadline)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_until(lock, deadline, [this]() {
            return m_ready;
        });
    }

    Try<T> waitAndTake()
    {
        wait();
        return takeResult();
    }

    Try<T> takeResult()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
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

    static void invokeScheduleFailure(const ScheduleFailure &handler) noexcept
    {
        try {
            if (handler) {
                handler();
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
    ScheduleFailure m_scheduleFailure;
    std::shared_ptr<Executor> m_executor;
    InterruptHandler m_interruptHandler;
    std::exception_ptr m_interrupt;
};
}
