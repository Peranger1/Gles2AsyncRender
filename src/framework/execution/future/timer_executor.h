#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace async
{
namespace detail
{
struct TimerTask final
{
    using Clock = std::chrono::steady_clock;

    Clock::time_point deadline;
    std::size_t sequence = 0;
    std::function<void()> task;
    std::atomic<bool> cancelled { false };
};

struct TimerTaskCompare final
{
    bool operator()(const std::shared_ptr<TimerTask> &lhs,
                    const std::shared_ptr<TimerTask> &rhs) const
    {
        if (lhs->deadline == rhs->deadline) {
            return lhs->sequence > rhs->sequence;
        }
        return lhs->deadline > rhs->deadline;
    }
};

struct TimerExecutorState final
{
    std::mutex mutex;
    std::condition_variable cv;
    std::priority_queue<std::shared_ptr<TimerTask>,
                        std::vector<std::shared_ptr<TimerTask>>,
                        TimerTaskCompare> tasks;
    bool stopping = false;
    // deadline 相同时用 sequence 保持稳定顺序，避免 priority_queue 中同 deadline 任务抖动。
    std::size_t nextSequence = 0;
};
}

// TimerTaskHandle 只取消尚未执行的 timer；已触发的任务不会被回滚。
class TimerTaskHandle final
{
public:
    TimerTaskHandle() = default;
    TimerTaskHandle(TimerTaskHandle &&other) noexcept
        : m_task(std::move(other.m_task))
        , m_state(std::move(other.m_state))
        , m_scheduled(other.m_scheduled)
    {
        other.m_scheduled = false;
    }

    TimerTaskHandle &operator=(TimerTaskHandle &&other) noexcept
    {
        if (this != &other) {
            m_task = std::move(other.m_task);
            m_state = std::move(other.m_state);
            m_scheduled = other.m_scheduled;
            other.m_scheduled = false;
        }
        return *this;
    }

    TimerTaskHandle(const TimerTaskHandle &) = delete;
    TimerTaskHandle &operator=(const TimerTaskHandle &) = delete;

    bool valid() const
    {
        if (!m_scheduled) {
            return false;
        }

        std::shared_ptr<detail::TimerTask> task = m_task.lock();
        return task && !task->cancelled.load();
    }

    bool scheduled() const
    {
        return m_scheduled;
    }

    void cancel()
    {
        std::shared_ptr<detail::TimerTask> task = m_task.lock();
        if (!task) {
            return;
        }

        task->cancelled.store(true);
        std::shared_ptr<detail::TimerExecutorState> state = m_state.lock();
        if (state) {
            // 唤醒 worker，让它尽快跳过已取消的堆顶任务。
            state->cv.notify_one();
        }
    }

private:
    friend class TimerExecutor;

    TimerTaskHandle(std::weak_ptr<detail::TimerTask> task,
                    std::weak_ptr<detail::TimerExecutorState> state)
        : m_task(std::move(task))
        , m_state(std::move(state))
        , m_scheduled(true)
    {
    }

    std::weak_ptr<detail::TimerTask> m_task;
    std::weak_ptr<detail::TimerExecutorState> m_state;
    bool m_scheduled = false;
};

class TimerExecutor final
{
public:
    using Clock = std::chrono::steady_clock;

    TimerExecutor()
        : m_state(std::make_shared<detail::TimerExecutorState>())
        , m_worker([state = m_state]() {
            runWorker(state);
        })
    {
    }

    ~TimerExecutor()
    {
        shutdown();
    }

    TimerExecutor(const TimerExecutor &) = delete;
    TimerExecutor &operator=(const TimerExecutor &) = delete;

    TimerTaskHandle scheduleAt(Clock::time_point deadline, std::function<void()> task)
    {
        if (!task) {
            return {};
        }

        auto timerTask = std::make_shared<detail::TimerTask>();
        timerTask->deadline = deadline;
        timerTask->task = std::move(task);

        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->stopping) {
                return {};
            }

            timerTask->sequence = m_state->nextSequence++;
            m_state->tasks.push(timerTask);
        }

        m_state->cv.notify_one();
        return TimerTaskHandle(timerTask, m_state);
    }

    template <typename Rep, typename Period>
    TimerTaskHandle scheduleAfter(std::chrono::duration<Rep, Period> delay,
                                  std::function<void()> task)
    {
        Clock::time_point deadline = Clock::now();
        if (delay > delay.zero()) {
            deadline += std::chrono::duration_cast<Clock::duration>(delay);
        }
        return scheduleAt(deadline, std::move(task));
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->stopping) {
                return;
            }
            m_state->stopping = true;
            while (!m_state->tasks.empty()) {
                m_state->tasks.top()->cancelled.store(true);
                m_state->tasks.pop();
            }
        }
        m_state->cv.notify_all();

        if (!m_worker.joinable()) {
            return;
        }
        if (m_worker.get_id() == std::this_thread::get_id()) {
            m_worker.detach();
            return;
        }
        m_worker.join();
    }

    bool isShutdown() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->stopping;
    }

    int queuedCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return int(m_state->tasks.size());
    }

    const char *typeName() const
    {
        return "TimerExecutor";
    }

    static std::shared_ptr<TimerExecutor> instance()
    {
        static std::shared_ptr<TimerExecutor> executor = std::make_shared<TimerExecutor>();
        return executor;
    }

private:
    static void runWorker(const std::shared_ptr<detail::TimerExecutorState> &state)
    {
        while (true) {
            std::shared_ptr<detail::TimerTask> task;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                while (true) {
                    if (state->stopping && state->tasks.empty()) {
                        return;
                    }

                    if (state->tasks.empty()) {
                        state->cv.wait(lock, [&]() {
                            return state->stopping || !state->tasks.empty();
                        });
                        continue;
                    }

                    std::shared_ptr<detail::TimerTask> next = state->tasks.top();
                    if (next->cancelled.load()) {
                        // 取消采用懒删除，worker 取到堆顶时再丢弃。
                        state->tasks.pop();
                        continue;
                    }

                    Clock::time_point now = Clock::now();
                    if (now < next->deadline) {
                        state->cv.wait_until(lock, next->deadline);
                        continue;
                    }

                    task = next;
                    state->tasks.pop();
                    break;
                }
            }

            if (task && !task->cancelled.exchange(true)) {
                try {
                    task->task();
                } catch (...) {
                }
            }
        }
    }

    std::shared_ptr<detail::TimerExecutorState> m_state;
    std::thread m_worker;
};
}
