
#pragma once

#include <cstdint>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>

#include "Task.h"
#include "TaskQueue.h"
#include "WheelTimer.h"
#include "TimerTrace.h"
#include "WakeSignal.h"
#include "WaitReturnStatistics.h"
#include <memory>

class Worker;

struct TickerIdleState
{
    size_t readyWork = 0;
    size_t activeRunners = 0;
    bool runnable = false;
};

class Ticker
{
public:
    virtual ~Ticker() = default;

    // 모든 작업을 마치면 true, 남은 작업이 있다면 false
    virtual bool on_tick() = 0;
    virtual bool has_work() const = 0;
    virtual TickerIdleState idle_state() const
    {
        return {};
    }
    // Only opt in when every transition to runnable work can notify attached Workers.
    virtual bool supports_notifications() const
    {
        return false;
    }
    virtual void attach(Worker*)
    {
    }
    virtual void detach(Worker*)
    {
    }
};

struct WorkerStatistics
{
    uint64_t immediateTasks = 0;
    uint64_t timerTasks = 0;
    uint64_t cancelledTasks = 0;
    uint64_t pendingTasks = 0;
    uint64_t pendingTimers = 0;
    uint64_t maxQueueDepth = 0;
    uint64_t maxQueueDelayMs = 0;
    uint64_t maxTimerDelayMs = 0;
    uint64_t busyNs = 0;
    uint64_t idleNs = 0;
    uint64_t waitCount = 0;
    uint64_t retainedTimerBytes = 0;
    uint64_t sleepReadySamples = 0;
    uint64_t sleepBlockedSamples = 0;
    uint64_t sleepPastTimerCount = 0;
    uint64_t sleepPastTimerMs = 0;
    uint64_t sleepOvershootNs = 0;
    uint64_t sleepWakeRequests = 0;
    uint64_t signalWaits = 0;
    uint64_t notifiedWaits = 0;
    uint64_t wakeLatencyNs = 0;
    uint64_t maxWakeLatencyNs = 0;
    uint64_t timerPhaseNs = 0;
    uint64_t requestPhaseNs = 0;
    uint64_t tickerPhaseNs = 0;
    uint64_t blockedIdleNs = 0;
};

class Worker
{
    static constexpr size_t REQUEST_BATCH_LIMIT = 256;
    std::thread m_thread;
    std::atomic_bool m_running{false};

    std::atomic_bool m_accepting{true};
    std::atomic_bool m_started{false};
    std::atomic_bool m_waiting{false};
    WakeSignal m_wake;
    bool m_pollingWait = false;
    bool m_diagnosticsEnabled = true;
    std::atomic_int64_t m_waitDeadline{(std::numeric_limits<int64_t>::max)()};
    std::atomic_size_t m_submitters{0};
    TaskQueue m_requests;
    Task* m_localRequests = nullptr;
    WheelTimer m_timer;
    std::atomic_uint64_t m_immediateTasks{0}, m_timerTasks{0}, m_cancelledTasks{0};
    std::atomic_uint64_t m_pendingTasks{0}, m_pendingTimers{0}, m_queueDepth{0};
    std::atomic_uint64_t m_maxQueueDepth{0}, m_maxQueueDelayMs{0}, m_maxTimerDelayMs{0};
    std::atomic_uint64_t m_busyNs{0}, m_idleNs{0}, m_waitCount{0};
    std::atomic_uint64_t m_retainedTimerBytes{0};
    std::atomic_uint64_t m_sleepReadySamples{0}, m_sleepBlockedSamples{0};
    std::atomic_uint64_t m_sleepPastTimerCount{0}, m_sleepPastTimerMs{0};
    std::atomic_uint64_t m_sleepOvershootNs{0}, m_sleepWakeRequests{0};
    std::atomic_uint64_t m_signalWaits{0}, m_notifiedWaits{0};
    std::atomic_uint64_t m_wakeLatencyNs{0}, m_maxWakeLatencyNs{0};
    std::atomic_uint64_t m_timerPhaseNs{0}, m_requestPhaseNs{0}, m_tickerPhaseNs{0}, m_blockedIdleNs{0};
    uint64_t m_timerTraceThresholdMs = 0;
    TimerTraceBuffer m_timerTraces;
    int64_t m_lastPollTick = 0, m_lastTimerEndTick = 0;
    int64_t m_lastRequestBeginTick = 0, m_lastRequestEndTick = 0;
    int64_t m_lastTickerBeginTick = 0, m_lastTickerEndTick = 0;
    int64_t m_lastSleepBeginTick = 0, m_lastSleepEndTick = 0;
#ifdef AREAUPDATE_TEST_HOOKS
    void (*m_submissionHook)(void*) = nullptr;
    void* m_submissionHookContext = nullptr;
    void (*m_waitHook)(void*) = nullptr;
    void* m_waitHookContext = nullptr;
#endif

    Ticker* m_ticker = nullptr;
    size_t m_id = 0;
    std::unique_ptr<WaitReturnStatistics> m_waitReturns{};

public:
    Worker() = default;
    ~Worker();

    void start(Ticker* ticker, size_t id);
    void stop();

    void join();
    bool wake();
    bool is_running() const
    {
        return m_running.load();
    }
    WorkerStatistics statistics() const;
    // Configure before start; read trace snapshots only after join.
    void set_timer_trace_threshold(uint64_t thresholdMs);
    void set_polling_wait(bool enabled);
    // Configure before submissions/start; OFF disables queue/timer delay diagnostics only.
    void set_runtime_diagnostics(bool enabled);
    void set_wait_return_diagnostics(bool enabled);
    // Worker-owned statistics are available only before start or after join.
    WaitReturnStatistics wait_return_statistics() const;
    TimerTraceSummary timer_trace_summary() const;
    std::vector<TimerDelayTrace> timer_traces() const;
#ifdef AREAUPDATE_TEST_HOOKS
    void set_submission_hook(void (*hook)(void*), void* context)
    {
        m_submissionHook = hook;
        m_submissionHookContext = context;
    }
    void set_wait_hook(void (*hook)(void*), void* context)
    {
        m_waitHook = hook;
        m_waitHookContext = context;
    }
#endif

    size_t get_id() const
    {
        return m_id;
    }

    // Ownership transfers on submission, including rejected tasks after stop.
    bool add_task(Task* task);
    bool add_task(Task* task, int64_t timeout);
    // Absolute deadline in the same millisecond clock as get_now_tick().
    bool add_task_at(Task* task, int64_t expireTick);

private:
    bool submit(Task* task, int64_t expire);
    void cleanup();

    void run();

    void wait_for_work();
    bool has_ready_work() const;

    void run_timer(int64_t nowTick);
    void run_task();
};

extern thread_local Worker* currentWorker;
