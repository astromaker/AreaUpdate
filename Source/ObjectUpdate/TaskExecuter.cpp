#include "TaskExecuter.h"
#include <deque>
#include "WakeSignal.h"
#include <stdexcept>
#include <algorithm>
#include <chrono>

namespace
{
    thread_local bool executingTasks = false;
    thread_local TaskDispatcher* currentTaskDispatcher = nullptr;
    thread_local std::deque<std::shared_ptr<TaskExecuter>> deferredTaskExecuters;
    thread_local DispatcherWorkerStatistics* currentDispatcherStatistics = nullptr;

    using DispatcherClock = std::chrono::steady_clock;
    uint64_t dispatcher_nanoseconds(DispatcherClock::duration value)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
    }

} // namespace

struct TaskDispatcher::WorkerState
{
    TaskQueue m_tasks;
    WakeSignal m_wake;
    std::atomic_bool m_waiting{false};
    std::atomic_bool m_running{true};
    std::thread m_thread;
    std::unique_ptr<DispatcherWorkerStatistics> m_statistics{};
};

TaskDispatcher::TaskDispatcher(size_t workerCount, uint64_t profileStride)
    : m_profileStride(profileStride)
{
    if (workerCount == 0)
        throw std::invalid_argument("workerCount must be positive");

    for (size_t i = 0; i < workerCount; ++i)
    {
        auto worker = std::make_unique<WorkerState>();
        if (m_profileStride != 0)
            worker->m_statistics = std::make_unique<DispatcherWorkerStatistics>();
        m_workers.emplace_back(std::move(worker));
    }

    try
    {
        for (auto& worker : m_workers)
        {
            worker->m_thread = std::thread(
                [this, pointer = worker.get()]
                {
                    run_worker(*pointer);
                });
        }
    }
    catch (...)
    {
        for (auto& worker : m_workers)
        {
            worker->m_running = false;
            worker->m_wake.signal();
        }

        for (auto& worker : m_workers)
        {
            if (worker->m_thread.joinable())
                worker->m_thread.join();
        }

        throw;
    }
}

TaskDispatcher::~TaskDispatcher()
{
    shutdown();
}

void TaskDispatcher::shutdown()
{
    if (m_shutdown)
        return;

    wait_idle();

    for (auto& worker : m_workers)
    {
        worker->m_running = false;
        worker->m_wake.signal();
    }

    for (auto& worker : m_workers)
        worker->m_thread.join();
    m_shutdown = true;
}

std::vector<DispatcherWorkerStatistics> TaskDispatcher::worker_statistics() const
{
    if (m_shutdown == false)
        throw std::logic_error("Read Dispatcher statistics only after shutdown");

    std::vector<DispatcherWorkerStatistics> result;
    result.reserve(m_workers.size());
    for (const auto& worker : m_workers)
        result.push_back(worker->m_statistics != nullptr ? *worker->m_statistics : DispatcherWorkerStatistics{});

    return result;
}

void TaskDispatcher::run_worker(WorkerState& worker)
{
    currentTaskDispatcher = this;
    const bool profiling = m_profileStride != 0;
    auto statistics = worker.m_statistics.get();
    currentDispatcherStatistics = statistics;
    const auto workerBegin = profiling ? DispatcherClock::now() : DispatcherClock::time_point{};

    while (worker.m_running)
    {
        auto list = worker.m_tasks.pop_all();

        if (list == nullptr)
        {
            // Recheck after publishing waiting, so a producer cannot lose a wake-up.
            worker.m_waiting = true;
            list = worker.m_tasks.pop_all();

            if (list == nullptr && worker.m_running)
            {
                const auto waitBegin = profiling ? DispatcherClock::now() : DispatcherClock::time_point{};
                worker.m_wake.wait();
                if (profiling)
                {
                    ++statistics->waits;
                    statistics->idleNs += dispatcher_nanoseconds(DispatcherClock::now() - waitBegin);
                }
            }
            worker.m_waiting = false;
        }

        const auto batchBegin = profiling ? DispatcherClock::now() : DispatcherClock::time_point{};
        uint64_t batchTasks = 0;
        while (list != nullptr)
        {
            auto next = TaskQueue::next(list);
            const bool sampled = profiling && list->m_submittedTick >= 0;
            const auto submittedTick = sampled ? list->m_submittedTick : int64_t{0};
            const auto runBegin = sampled ? DispatcherClock::now() : DispatcherClock::time_point{};

            list->run();
            delete list;
            list = next;
            if (sampled)
            {
                const auto duration = dispatcher_nanoseconds(DispatcherClock::now() - runBegin);
                const auto beginMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(runBegin.time_since_epoch()).count();
                const auto delay = static_cast<uint64_t>((std::max)(int64_t{0}, beginMs - submittedTick));
                ++statistics->samples;
                statistics->queueDelayMs += delay;
                statistics->maxQueueDelayMs = (std::max)(statistics->maxQueueDelayMs, delay);
                ++statistics->queueHistogram[(std::min)(delay, uint64_t{200})];
                statistics->sampledRunNs += duration;
                statistics->maxSampledRunNs = (std::max)(statistics->maxSampledRunNs, duration);
            }
            ++batchTasks;

            if (m_pendingTasks.fetch_sub(1) == 1)
            {
                std::lock_guard<std::mutex> guard(m_idleLock);
                m_idle.notify_all();
            }
        }
        if (profiling && batchTasks != 0)
        {
            statistics->dispatchTasks += batchTasks;
            ++statistics->batches;
            statistics->maxBatchTasks = (std::max)(statistics->maxBatchTasks, batchTasks);
            statistics->busyNs += dispatcher_nanoseconds(DispatcherClock::now() - batchBegin);
        }
    }

    if (profiling)
        statistics->elapsedNs = dispatcher_nanoseconds(DispatcherClock::now() - workerBegin);
    currentDispatcherStatistics = nullptr;
    currentTaskDispatcher = nullptr;
}

void TaskDispatcher::post(std::function<void()> function)
{
    post(std::make_unique<FunctionTask>(std::move(function)));
}

void TaskDispatcher::post(std::unique_ptr<Task> task)
{
    if (task == nullptr)
        throw std::invalid_argument("Cannot dispatch a null Task");
    if (m_shutdown)
        throw std::logic_error("Dispatcher is shut down");

    ++m_pendingTasks;
    // Selection needs a unique sequence number, not publication ordering.
    const auto sequence = m_nextWorker.fetch_add(1, std::memory_order_relaxed);
    auto& worker = *m_workers[sequence % m_workers.size()];
    if (m_profileStride != 0)
    {
        // Sample each Worker's own turns, avoiding stride/Worker-count aliasing.
        const bool sampled = (sequence / m_workers.size()) % m_profileStride == 0;
        task->m_submittedTick = sampled ? get_now_tick() : int64_t{-1};
    }
    worker.m_tasks.push(task.release());

    if (worker.m_waiting.exchange(false))
        worker.m_wake.signal();
}

void TaskDispatcher::wait_idle()
{
    std::unique_lock<std::mutex> guard(m_idleLock);

    m_idle.wait(guard,
                [this]
                {
                    return m_pendingTasks.load() == 0;
                });
}

void TaskExecuter::run_task(Task* task)
{
    if (m_dispatcher.is_shutdown())
    {
        delete task;
        throw std::logic_error("Dispatcher is shut down");
    }
    // Keep the original count-before-publication protocol and 0 -> 1 owner election.
    const auto count = m_remainingTaskCount.fetch_add(1) + 1;
    m_taskQueue.push(task);

    if (count != 1)
        return;

    if (currentTaskDispatcher == &m_dispatcher)
    {
        try_execute();
    }
    else
    {
        post_execute();
    }
}

class TaskExecuter::ExecuteTask final : public Task
{
    std::shared_ptr<TaskExecuter> m_executer;

public:
    explicit ExecuteTask(std::shared_ptr<TaskExecuter> executer)
        : m_executer(std::move(executer))
    {
    }
    void run() override
    {
        m_executer->try_execute();
    }
};

void TaskExecuter::post_execute()
{
    m_dispatcher.post(std::make_unique<ExecuteTask>(shared_from_this()));
}

void TaskExecuter::try_execute()
{
    if (executingTasks)
    {
        if (deferredTaskExecuters.empty())
        {
            deferredTaskExecuters.push_back(shared_from_this());
        }
        else
        {
            post_execute();
        }
        return;
    }

    executingTasks = true;
    execute();

    while (deferredTaskExecuters.empty() == false)
    {
        auto next = std::move(deferredTaskExecuters.front());
        deferredTaskExecuters.pop_front();
        next->execute();
    }

    executingTasks = false;
}

void TaskExecuter::execute()
{
    auto list = m_taskQueue.pop_all();
    if (list == nullptr)
    {
        post_execute();
        return;
    }

    size_t executed = 0;
    while (list != nullptr)
    {
        auto next = TaskQueue::next(list);
        list->run();
        delete list;
        list = next;
        ++executed;
    }

    // Exactly one detached batch per turn, as in PWJobExecuter::LOOP_COUNT = 1.
    if (currentDispatcherStatistics != nullptr)
        currentDispatcherStatistics->executerTasks += executed;
    if (m_remainingTaskCount.fetch_sub(executed) != executed)
        post_execute();
}
