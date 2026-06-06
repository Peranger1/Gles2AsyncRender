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
                m_state->active = true;
                shouldStart = true;
            } else {
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
