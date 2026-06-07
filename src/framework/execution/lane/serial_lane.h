#pragma once

#include "framework/execution/future/async_future.h"
#include "framework/execution/lane/lane_exceptions.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace execution
{
template <typename Args, typename Result>
class SerialLane final
{
public:
    // runner 必须返回有效 future；要拒绝任务时返回 failed future，而不是 invalid future。
    using Runner = std::function<async::Future<Result>(Args)>;

    explicit SerialLane(Runner runner)
        : m_state(std::make_shared<State>(std::move(runner)))
    {
    }

    async::Future<Result> submit(Args args)
    {
        auto promise = std::make_shared<async::Promise<Result>>();
        async::Future<Result> future = promise->getFuture();

        Pending pending { std::move(args), promise };

        bool shouldStart = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shutdown || !m_state->runner) {
                promise->setException(std::make_exception_ptr(ExecutorShutdown()));
                return future;
            }

            if (!m_state->active) {
                // 没有 active 任务时，本次提交立即成为 active。
                m_state->active = true;
                shouldStart = true;
            } else {
                // active 未完成时只排队，不在锁内启动 runner。
                m_state->waiting.push_back(std::move(pending));
                return future;
            }
        }

        if (shouldStart) {
            start(m_state, std::move(pending));
        }
        return future;
    }

    void shutdown()
    {
        std::deque<Pending> waiting;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shutdown) {
                return;
            }
            m_state->shutdown = true;
            // shutdown 只拒绝等待队列，已经 active 的任务仍交给 runner future 完成。
            waiting = std::move(m_state->waiting);
            m_state->waiting.clear();
        }

        for (Pending &pending : waiting) {
            pending.promise->setException(std::make_exception_ptr(ExecutorShutdown()));
        }
    }

private:
    struct Pending final
    {
        Args args;
        std::shared_ptr<async::Promise<Result>> promise;
    };

    struct State final
    {
        explicit State(Runner runnerIn)
            : runner(std::move(runnerIn))
        {
        }

        std::mutex mutex;
        Runner runner;
        bool active = false;
        bool shutdown = false;
        std::deque<Pending> waiting;
    };

    static void start(const std::shared_ptr<State> &state, Pending pending)
    {
        async::Future<Result> work;
        try {
            // runner 在锁外执行，避免业务代码重入 lane 状态锁。
            work = state->runner(std::move(pending.args));
            if (!work.valid()) {
                throw async::FutureInvalid();
            }
        } catch (...) {
            pending.promise->setException(std::current_exception());
            startNext(state);
            return;
        }

        std::move(work).thenTry([state, promise = pending.promise](async::Try<Result> &&result) {
            promise->setTry(std::move(result));
            // active 完成后再尝试启动队首，保持严格 FIFO。
            startNext(state);
            return async::Unit();
        });
    }

    static void startNext(const std::shared_ptr<State> &state)
    {
        std::optional<Pending> next;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->shutdown && !state->waiting.empty()) {
                next = std::move(state->waiting.front());
                state->waiting.pop_front();
            } else {
                // 没有可启动的 waiting 时释放 active 标志，下一次 submit 可直接启动。
                state->active = false;
            }
        }

        if (next.has_value()) {
            start(state, std::move(*next));
        }
    }

    std::shared_ptr<State> m_state;
};
}
