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
enum class LatestLaneDelivery
{
    DeliverActiveResult,
    // 如果 active 完成时已经有 newer waiting，active 结果会被标记为 TaskStale。
    MarkActiveStaleWhenWaiting
};

template <typename Args,
          typename Result,
          LatestLaneDelivery Delivery = LatestLaneDelivery::DeliverActiveResult>
class LatestLane final
{
public:
    // runner 必须返回有效 future；LatestLane 只管理等待任务的替换策略。
    using Runner = std::function<async::Future<Result>(Args)>;

    explicit LatestLane(Runner runner)
        : m_state(std::make_shared<State>(std::move(runner)))
    {
    }

    async::Future<Result> submit(Args args)
    {
        auto promise = std::make_shared<async::Promise<Result>>();
        async::Future<Result> future = promise->getFuture();

        Pending pending { std::move(args), promise };

        bool shouldStart = false;
        std::optional<Pending> superseded;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shutdown || !m_state->runner) {
                promise->setException(std::make_exception_ptr(ExecutorShutdown()));
                return future;
            }

            if (!m_state->active) {
                m_state->active = true;
                shouldStart = true;
            } else {
                if (m_state->waiting.has_value()) {
                    // waiting 只保留最新一个；旧 waiting 在锁外兑现为 TaskSuperseded。
                    superseded = std::move(m_state->waiting);
                }
                m_state->waiting = std::move(pending);
            }
        }

        if (superseded.has_value()) {
            superseded->promise->setException(std::make_exception_ptr(TaskSuperseded()));
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
            // shutdown 不取消 active，只拒绝尚未启动的 latest waiting。
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
        explicit State(Runner runnerIn)
            : runner(std::move(runnerIn))
        {
        }

        std::mutex mutex;
        Runner runner;
        bool active = false;
        bool shutdown = false;
        std::optional<Pending> waiting;
    };

    static void start(const std::shared_ptr<State> &state, Pending pending)
    {
        async::Future<Result> work;
        try {
            // runner 可能抛异常或返回 invalid future；这两种情况都要清理 active 状态。
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
            if (isActiveStale(state)) {
                promise->setException(std::make_exception_ptr(TaskStale()));
            } else {
                promise->setTry(std::move(result));
            }
            // 无论 active 结果是否 stale，都要继续启动最新 waiting。
            startNext(state);
            return async::Unit();
        });
    }

    static bool isActiveStale(const std::shared_ptr<State> &state)
    {
        if constexpr (Delivery == LatestLaneDelivery::DeliverActiveResult) {
            return false;
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->waiting.has_value();
        }
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
