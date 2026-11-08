#pragma once

#include "TaskQueue.h"
#include "FunctionTask.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <array>

struct DispatcherWorkerStatistics
{
    uint64_t dispatchTasks = 0, executerTasks = 0, batches = 0, maxBatchTasks = 0;
    uint64_t busyNs = 0, idleNs = 0, elapsedNs = 0, waits = 0;
    uint64_t samples = 0, queueDelayMs = 0, maxQueueDelayMs = 0;
    uint64_t sampledRunNs = 0, maxSampledRunNs = 0;
    std::array<uint64_t, 201> queueHistogram{};
};

class TaskDispatcher
{
    struct WorkerState;
    std::vector<std::unique_ptr<WorkerState>> m_workers;
    std::atomic_size_t m_nextWorker{0};
    std::atomic_size_t m_pendingTasks{0};
    std::mutex m_idleLock;
    std::condition_variable m_idle;
    uint64_t m_profileStride = 0;
    std::atomic_bool m_shutdown{false};
    void run_worker(WorkerState& worker);

public:
    explicit TaskDispatcher(size_t workerCount, uint64_t profileStride = 0);
    ~TaskDispatcher();
    void post(std::function<void()> function);
    // Ownership transfers on entry; completion includes Task destruction.
    void post(std::unique_ptr<Task> task);
    void wait_idle();
    // Stop producers first; shutdown calls must not overlap each other or post/run_task.
    void shutdown();
    std::vector<DispatcherWorkerStatistics> worker_statistics() const;
    bool is_shutdown() const
    {
        return m_shutdown.load();
    }
};

class TaskExecuter : public std::enable_shared_from_this<TaskExecuter>
{
    TaskDispatcher& m_dispatcher;
    std::atomic_size_t m_remainingTaskCount{0};
    TaskQueue m_taskQueue;

public:
    explicit TaskExecuter(TaskDispatcher& dispatcher)
        : m_dispatcher(dispatcher)
    {
    }
    void run_task(Task* task);

private:
    class ExecuteTask;
    void try_execute();
    void execute();
    void post_execute();
};
