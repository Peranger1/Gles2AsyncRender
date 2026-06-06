#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace async
{
class Executor
{
public:
    virtual ~Executor() = default;
    virtual void add(std::function<void()> task) = 0;
};

class InlineExecutor final : public Executor
{
public:
    void add(std::function<void()> task) override
    {
        if (task) {
            task();
        }
    }

    static std::shared_ptr<Executor> instance()
    {
        static std::shared_ptr<Executor> executor = std::make_shared<InlineExecutor>();
        return executor;
    }
};

class ThreadExecutor final : public Executor
{
public:
    void add(std::function<void()> task) override
    {
        if (!task) {
            return;
        }
        std::thread(std::move(task)).detach();
    }
};

class ManualExecutor final : public Executor
{
public:
    void add(std::function<void()> task) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (task) {
            m_tasks.push(std::move(task));
        }
    }

    bool drainOne()
    {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_tasks.empty()) {
                return false;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        if (task) {
            task();
        }
        return true;
    }

    void drain()
    {
        while (drainOne()) {
        }
    }

    int queuedCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return int(m_tasks.size());
    }

    bool empty() const
    {
        return queuedCount() == 0;
    }

private:
    mutable std::mutex m_mutex;
    std::queue<std::function<void()>> m_tasks;
};

class SerialExecutor final : public Executor
{
public:
    explicit SerialExecutor(std::shared_ptr<Executor> underlying)
        : m_state(std::make_shared<State>(std::move(underlying)))
    {
    }

    void add(std::function<void()> task) override
    {
        bool shouldSchedule = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shutdown || !m_state->underlying || !task) {
                return;
            }
            m_state->tasks.push(std::move(task));
            if (!m_state->running) {
                m_state->running = true;
                shouldSchedule = true;
            }
        }

        if (shouldSchedule) {
            scheduleDrain(m_state);
        }
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->shutdown = true;
        while (!m_state->tasks.empty()) {
            m_state->tasks.pop();
        }
    }

    bool isShutdown() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->shutdown;
    }

private:
    struct State final
    {
        explicit State(std::shared_ptr<Executor> underlyingIn)
            : underlying(std::move(underlyingIn))
        {
        }

        std::mutex mutex;
        std::queue<std::function<void()>> tasks;
        std::shared_ptr<Executor> underlying;
        bool running = false;
        bool shutdown = false;
    };

    static void scheduleDrain(const std::shared_ptr<State> &state)
    {
        const std::shared_ptr<Executor> underlying = state->underlying;
        if (!underlying) {
            return;
        }

        underlying->add([state]() {
            drain(state);
        });
    }

    static void drain(const std::shared_ptr<State> &state)
    {
        while (true) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->shutdown || state->tasks.empty()) {
                    state->running = false;
                    return;
                }
                task = std::move(state->tasks.front());
                state->tasks.pop();
            }

            try {
                if (task) {
                    task();
                }
            } catch (...) {
            }
        }
    }

    std::shared_ptr<State> m_state;
};

class ThreadPoolExecutor final : public Executor
{
public:
    explicit ThreadPoolExecutor(std::size_t threadCount = std::thread::hardware_concurrency())
    {
        if (threadCount == 0) {
            threadCount = 1;
        }

        for (std::size_t i = 0; i < threadCount; ++i) {
            m_workers.emplace_back([this]() {
                runWorker();
            });
        }
    }

    ~ThreadPoolExecutor() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();

        for (std::thread &worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ThreadPoolExecutor(const ThreadPoolExecutor &) = delete;
    ThreadPoolExecutor &operator=(const ThreadPoolExecutor &) = delete;

    void add(std::function<void()> task) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping || !task) {
                return;
            }
            m_tasks.push(std::move(task));
        }
        m_cv.notify_one();
    }

private:
    void runWorker()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return m_stopping || !m_tasks.empty();
                });

                if (m_stopping && m_tasks.empty()) {
                    return;
                }

                task = std::move(m_tasks.front());
                m_tasks.pop();
            }

            if (task) {
                task();
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_tasks;
    std::vector<std::thread> m_workers;
    bool m_stopping = false;
};

class SingleThreadExecutor final : public Executor
{
public:
    explicit SingleThreadExecutor(std::string name = {})
        : m_name(std::move(name))
        , m_worker([this]() {
            runWorker();
        })
    {
    }

    ~SingleThreadExecutor() override
    {
        shutdown();
    }

    SingleThreadExecutor(const SingleThreadExecutor &) = delete;
    SingleThreadExecutor &operator=(const SingleThreadExecutor &) = delete;

    void add(std::function<void()> task) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping || !task) {
                return;
            }
            m_tasks.push(std::move(task));
        }
        m_cv.notify_one();
    }

    bool isOnExecutorThread() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_threadId == std::this_thread::get_id();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
        }
        m_cv.notify_all();

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
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping;
    }

    const std::string &name() const
    {
        return m_name;
    }

private:
    void runWorker()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_threadId = std::this_thread::get_id();
        }

        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return m_stopping || !m_tasks.empty();
                });

                if (m_stopping && m_tasks.empty()) {
                    return;
                }

                task = std::move(m_tasks.front());
                m_tasks.pop();
            }

            if (task) {
                task();
            }
        }
    }

    std::string m_name;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_tasks;
    std::thread m_worker;
    std::thread::id m_threadId;
    bool m_stopping = false;
};

namespace detail
{
inline void schedule(const std::shared_ptr<Executor> &executor, std::function<void()> task)
{
    if (executor) {
        executor->add(std::move(task));
    } else {
        InlineExecutor::instance()->add(std::move(task));
    }
}
}
}
