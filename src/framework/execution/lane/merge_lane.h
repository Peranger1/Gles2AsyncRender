#pragma once

#include "framework/execution/future/async_future.h"
#include "framework/execution/lane/lane_exceptions.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace execution
{
template <typename Args, typename Result, typename Merger>
class MergeLane final
{
public:
    // Merger 决定 waiting 任务能否合并；active 任务始终不会被合并或替换。
    using Runner = std::function<async::Future<Result>(Args)>;

    MergeLane(Runner runner, Merger merger)
        : m_state(std::make_shared<State>(std::move(runner), std::move(merger)))
    {
    }

    async::Future<Result> submit(Args args)
    {
        auto promise = std::make_shared<async::Promise<Result>>();
        async::Future<Result> future = promise->getFuture();

        Pending pending { std::move(args), promise };

        bool shouldStart = false;
        std::optional<Pending> superseded;
        bool rejected = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shutdown || !m_state->runner) {
                promise->setException(std::make_exception_ptr(ExecutorShutdown()));
                return future;
            }

            if (!m_state->active) {
                m_state->active = true;
                shouldStart = true;
            } else if (!m_state->waiting.has_value()) {
                m_state->waiting = std::move(pending);
            } else if (m_state->merger.canMerge(m_state->waiting->args, pending.args)) {
                // 合并成功时旧 waiting future 以 TaskSuperseded 结束，新 waiting 继承合并后的 args。
                superseded = std::move(m_state->waiting);
                pending.args = m_state->merger.merge(superseded->args, std::move(pending.args));
                m_state->waiting = std::move(pending);
            } else {
                rejected = true;
            }
        }

        if (superseded.has_value()) {
            superseded->promise->setException(std::make_exception_ptr(TaskSuperseded()));
        }
        if (rejected) {
            promise->setException(std::make_exception_ptr(TaskRejected()));
        }
        if (shouldStart) {
            start(m_state, std::move(pending));
        }
        return future;
    }

    void shutdown()
    {
        std::optional<Pending> waiting;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shutdown) {
                return;
            }
            m_state->shutdown = true;
            // shutdown 只拒绝未启动的 waiting；active 任务继续由 runner future 兑现。
            waiting = std::move(m_state->waiting);
            m_state->waiting.reset();
        }

        if (waiting.has_value()) {
            waiting->promise->setException(std::make_exception_ptr(ExecutorShutdown()));
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
        State(Runner runnerIn, Merger mergerIn)
            : runner(std::move(runnerIn))
            , merger(std::move(mergerIn))
        {
        }

        std::mutex mutex;
        Runner runner;
        Merger merger;
        bool active = false;
        bool shutdown = false;
        std::optional<Pending> waiting;
    };

    static void start(const std::shared_ptr<State> &state, Pending pending)
    {
        async::Future<Result> work;
        try {
            // runner 在锁外执行；异常和 invalid future 都转为提交方可观察的 failed future。
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
            // active 完成后最多启动一个合并后的 waiting。
            startNext(state);
            return async::Unit();
        });
    }

    static void startNext(const std::shared_ptr<State> &state)
    {
        std::optional<Pending> next;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->shutdown && state->waiting.has_value()) {
                next = std::move(*state->waiting);
                state->waiting.reset();
            } else {
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
