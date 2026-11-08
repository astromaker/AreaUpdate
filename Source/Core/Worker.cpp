
#include "Worker.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

thread_local Worker* currentWorker = nullptr;

namespace
{
    using Clock = std::chrono::steady_clock;
    void update_max(std::atomic_uint64_t& value, uint64_t candidate)
    {
        auto old = value.load(std::memory_order_relaxed);

        while (candidate > old && value.compare_exchange_weak(old, candidate, std::memory_order_relaxed) == false)
        {
        }
    }
    uint64_t nanoseconds(Clock::duration value)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
    }
} // namespace

Worker::~Worker()
{
    stop();
    join();
    cleanup();
}

void Worker::start(Ticker* ticker, size_t id)
{
    if (m_started.exchange(true) || m_accepting.load() == false)
        throw std::logic_error("Worker starts only once");

    m_ticker = ticker;
    m_id = id;

    if (m_ticker != nullptr)
        m_ticker->attach(this);

    m_running = true;

    try
    {
        m_thread = std::thread(&Worker::run, this);
    }
    catch (...)
    {
        stop();
        if (m_ticker != nullptr)
            m_ticker->detach(this);

        throw;
    }
}

void Worker::stop()
{
    m_accepting.store(false);
    m_running.store(false);
    wake();
}

void Worker::join()
{
    if (m_thread.joinable())
        m_thread.join();
}

bool Worker::wake()
{
    if (m_waiting.exchange(false) == false)
        return false;

    if (m_pollingWait == false)
        m_wake.signal(Clock::now());

    return true;
}

bool Worker::add_task(Task* task)
{
    return submit(task, -1);
}

bool Worker::add_task(Task* task, int64_t timeout)
{
    const auto now = get_now_tick();
    const auto maximum = (std::numeric_limits<int64_t>::max)();
    const auto delay = (std::max)(int64_t{0}, timeout);

    return submit(task, delay > maximum - now ? maximum : now + delay);
}

bool Worker::add_task_at(Task* task, int64_t expireTick)
{
    return submit(task, (std::max)(int64_t{0}, expireTick));
}

void Worker::set_timer_trace_threshold(uint64_t thresholdMs)
{
    if (m_started.load())
        throw std::logic_error("Configure timer tracing before start");
    if (thresholdMs != 0 && m_diagnosticsEnabled == false)
        throw std::logic_error("Timer tracing requires runtime diagnostics");
    m_timerTraceThresholdMs = thresholdMs;
}

void Worker::set_runtime_diagnostics(bool enabled)
{
    if (m_started.load())
        throw std::logic_error("Configure diagnostics before start");
    if (m_pendingTasks.load() != 0)
        throw std::logic_error("Configure diagnostics before submitting work");
    if (enabled == false && m_timerTraceThresholdMs != 0)
        throw std::logic_error("Disable timer tracing before diagnostics");
    m_diagnosticsEnabled = enabled;
}

void Worker::set_wait_return_diagnostics(bool enabled)
{
    if (m_started.load() || m_pendingTasks.load() != 0 || m_submitters.load() != 0)
        throw std::logic_error("Configure wait return diagnostics before submissions/start");

    m_waitReturns = enabled ? std::make_unique<WaitReturnStatistics>() : nullptr;
    if (m_waitReturns != nullptr)
        m_waitReturns->enabled = true;
}

WaitReturnStatistics Worker::wait_return_statistics() const
{
    if (m_running.load() || m_thread.joinable())
        throw std::logic_error("Read wait return diagnostics only after join");
    return m_waitReturns != nullptr ? *m_waitReturns : WaitReturnStatistics{};
}

void Worker::set_polling_wait(bool enabled)
{
    if (m_started.load())
        throw std::logic_error("Configure waiting before start");

    m_pollingWait = enabled;
}

TimerTraceSummary Worker::timer_trace_summary() const
{
    if (m_thread.joinable() || m_running.load())
        throw std::logic_error("Read timer traces only after join");
    return m_timerTraces.summary();
}

std::vector<TimerDelayTrace> Worker::timer_traces() const
{
    if (m_thread.joinable() || m_running.load())
        throw std::logic_error("Read timer traces only after join");
    return m_timerTraces.events();
}

bool Worker::submit(Task* task, int64_t expire)
{
    if (task == nullptr)
        return false;

    ++m_submitters;

    if (m_accepting.load() == false)
    {
        --m_submitters;
        delete task;
        return false;
    }

    task->m_submittedTick = m_diagnosticsEnabled ? get_now_tick() : int64_t{0};
#ifdef AREAUPDATE_TEST_HOOKS
    if (m_submissionHook != nullptr)
        m_submissionHook(m_submissionHookContext);
#endif
    task->m_expireTick = expire;

    ++m_pendingTasks;
    if (expire >= 0)
        ++m_pendingTimers;

    if (m_diagnosticsEnabled)
        update_max(m_maxQueueDepth, ++m_queueDepth);

    m_requests.push(task);
    // Later timer registrations can be batched at the already scheduled wake-up.
    if (m_pollingWait || expire < 0 || expire < m_waitDeadline.load())
        wake();
    --m_submitters;

    return true;
}

void Worker::cleanup()
{
    // A producer accepted before stop may still be publishing its request.
    while (m_submitters.load() != 0)
        std::this_thread::yield();

    auto clear = [](Task* task)
    {
        while (task != nullptr)
        {
            auto next = task->m_next;
            delete task;
            task = next;
        }
    };

    clear(m_localRequests);
    m_localRequests = nullptr;
    clear(m_requests.pop_all());
    m_timer.clear();

    m_retainedTimerBytes.store(m_timer.retained_bytes());
    m_cancelledTasks.fetch_add(m_pendingTasks.exchange(0));
    m_pendingTimers = 0;
    m_queueDepth = 0;

    if (currentWorker == this)
        currentWorker = nullptr;
}

void Worker::run()
{
    currentWorker = this;

    while (m_running == true)
    {
        const auto begin = Clock::now();

        run_timer(get_now_tick());
        const auto timerEnd = m_timerTraceThresholdMs != 0 ? Clock::now() : Clock::time_point{};
        if (m_timerTraceThresholdMs != 0)
            m_lastRequestBeginTick = get_now_tick();
        run_task();
        const auto requestEnd = m_timerTraceThresholdMs != 0 ? Clock::now() : Clock::time_point{};
        if (m_timerTraceThresholdMs != 0)
        {
            m_lastRequestEndTick = get_now_tick();
            m_lastTickerBeginTick = 0;
            m_lastTickerEndTick = 0;
        }

        m_retainedTimerBytes.store(m_timer.retained_bytes(), std::memory_order_relaxed);
        if (m_ticker != nullptr)
        {
            if (m_timerTraceThresholdMs != 0)
                m_lastTickerBeginTick = get_now_tick();
            m_ticker->on_tick();
            if (m_timerTraceThresholdMs != 0)
                m_lastTickerEndTick = get_now_tick();
        }

        const auto end = Clock::now();
        if (m_timerTraceThresholdMs != 0)
        {
            m_timerPhaseNs += nanoseconds(timerEnd - begin);
            m_requestPhaseNs += nanoseconds(requestEnd - timerEnd);
            m_tickerPhaseNs += nanoseconds(end - requestEnd);
        }
        m_busyNs.fetch_add(nanoseconds(end - begin), std::memory_order_relaxed);

        if (m_running.load() == false)
            break;
        if (has_ready_work())
            continue;

        wait_for_work();
    }

    if (m_ticker != nullptr)
        m_ticker->detach(this);

    cleanup();
}

bool Worker::has_ready_work() const
{
    return m_localRequests != nullptr || m_requests.empty() == false || m_timer.next_expire_tick() <= get_now_tick()
           || (m_ticker != nullptr && m_ticker->has_work());
}

void Worker::wait_for_work()
{
    const auto deadline = m_timer.next_expire_tick();
    m_waitDeadline.store(deadline);
    m_waiting.store(true);

    // Publish waiting before rechecking; a signal in the gap remains retained.
    if (m_running.load() == false || has_ready_work())
    {
        m_waiting.store(false);
        return;
    }

#ifdef AREAUPDATE_TEST_HOOKS
    if (m_waitHook != nullptr)
        m_waitHook(m_waitHookContext);
#endif

    ++m_waitCount;
    const auto state = m_ticker != nullptr ? m_ticker->idle_state() : TickerIdleState{};
    if (state.runnable || m_requests.empty() == false || deadline <= get_now_tick())
    {
        ++m_sleepReadySamples;
    }
    else if (state.readyWork != 0)
    {
        ++m_sleepBlockedSamples;
    }

    if (m_timerTraceThresholdMs != 0)
        m_lastSleepBeginTick = get_now_tick();
    const auto begin = Clock::now();
    auto waitDeadline = begin + std::chrono::milliseconds(1);
    bool timedWait = true;
    WaitReturnKind returnKind = WaitReturnKind::Poll;
    Clock::time_point signalTime{}, observedReturn{};
    if (m_pollingWait)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    else
    {
        ++m_signalWaits;
        const bool notifications = m_ticker == nullptr || m_ticker->supports_notifications();
        const bool finite = deadline != (std::numeric_limits<int64_t>::max)();
        if (finite)
        {
            const auto maximumMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max().time_since_epoch())
                    .count();
            const auto timerDeadline = deadline < maximumMs ? Clock::time_point(std::chrono::milliseconds(deadline))
                                                            : Clock::time_point::max();
            waitDeadline = notifications ? timerDeadline : (std::min)(waitDeadline, timerDeadline);
        }

        if (notifications && finite == false)
        {
            timedWait = false;
            m_wake.wait(&signalTime);
            returnKind = WaitReturnKind::Untimed;
        }
        else
        {
            const bool signaled = m_wake.wait_until(waitDeadline, &signalTime);
            returnKind = signaled ? WaitReturnKind::Signaled : WaitReturnKind::Timeout;
        }

        if (signalTime != Clock::time_point{})
        {
            observedReturn = Clock::now();
            const auto latency = nanoseconds(observedReturn - signalTime);
            ++m_notifiedWaits;
            m_wakeLatencyNs.fetch_add(latency, std::memory_order_relaxed);
            update_max(m_maxWakeLatencyNs, latency);
        }
    }

    const auto waitEnd = Clock::now();
    const auto durationNs = nanoseconds(waitEnd - begin);
    if (observedReturn == Clock::time_point{})
        observedReturn = waitEnd;
    if (m_timerTraceThresholdMs != 0 && state.readyWork != 0 && state.runnable == false)
        m_blockedIdleNs += durationNs;
    const auto now = get_now_tick();
    if (m_waitReturns != nullptr)
    {
        const uint64_t overshoot =
            timedWait && observedReturn > waitDeadline ? nanoseconds(observedReturn - waitDeadline) : uint64_t{0};
        const uint64_t timerLate = deadline != (std::numeric_limits<int64_t>::max)() && now > deadline
                                       ? static_cast<uint64_t>(now - deadline)
                                       : uint64_t{0};
        auto& bucket = m_waitReturns->outcomes[static_cast<size_t>(returnKind)];
        bucket.observe(overshoot, timerLate);
        if (timedWait)
        {
            bucket.requestedWaitNs += nanoseconds((std::max)(Clock::duration::zero(), waitDeadline - begin));
            if (waitDeadline <= begin)
                ++bucket.pastDueAtBegin;
        }
        if (m_running.load() == false)
            ++bucket.stoppingReturns;
        if (signalTime != Clock::time_point{})
        {
            const auto latency = nanoseconds(observedReturn - signalTime);
            ++bucket.notificationSamples;
            bucket.notificationNs += latency;
            bucket.maxNotificationNs = (std::max)(bucket.maxNotificationNs, latency);
            if (timedWait && signalTime > waitDeadline)
            {
                ++bucket.lateSignalRequests;
            }
            else if (timedWait && overshoot != 0)
            {
                ++bucket.onTimeSignalLateReturns;
            }
        }
    }
    if (m_timerTraceThresholdMs != 0)
        m_lastSleepEndTick = now;
    if (deadline != (std::numeric_limits<int64_t>::max)() && now > deadline)
    {
        ++m_sleepPastTimerCount;
        m_sleepPastTimerMs.fetch_add(static_cast<uint64_t>(now - deadline), std::memory_order_relaxed);
    }
    const auto expectedNs = nanoseconds((std::max)(Clock::duration::zero(), waitDeadline - begin));
    if (timedWait && durationNs > expectedNs)
        m_sleepOvershootNs.fetch_add(durationNs - expectedNs, std::memory_order_relaxed);
    if (m_requests.empty() == false)
        ++m_sleepWakeRequests;

    m_idleNs.fetch_add(durationNs, std::memory_order_relaxed);
    m_waiting.store(false);
}

void Worker::run_timer(int64_t nowTick)
{
    const auto previousPoll = m_lastPollTick;
    const auto previousTimerEnd = m_lastTimerEndTick;
    if (m_timerTraceThresholdMs != 0)
        m_lastPollTick = nowTick;
    auto tasks = m_timer.expired_tasks(nowTick);
    const auto selectedTick = m_timerTraceThresholdMs != 0 ? get_now_tick() : int64_t{0};
    size_t batchIndex = 0;
    int64_t previousCallbackMs = 0, longestCallbackMs = 0;

    for (auto task : tasks)
    {
        const auto executeTick = m_diagnosticsEnabled ? get_now_tick() : int64_t{0};
        const auto delay = m_diagnosticsEnabled
                               ? static_cast<uint64_t>((std::max)(int64_t{0}, executeTick - task->m_expireTick))
                               : uint64_t{0};
        if (m_diagnosticsEnabled)
            update_max(m_maxTimerDelayMs, delay);
        if (m_timerTraceThresholdMs != 0)
        {
            m_timerTraces.observe(delay);
            if (delay >= m_timerTraceThresholdMs)
            {
                TimerDelayTrace event{};
                event.actorTag = task->trace_tag();
                event.delayMs = delay;
                event.submittedTick = task->m_submittedTick;
                event.deadlineTick = task->m_expireTick;
                event.insertedTick = task->m_timerInsertedTick;
                event.eligibleTick = task->m_wheelTick;
                event.pollTick = nowTick;
                event.selectedTick = selectedTick;
                event.executeTick = executeTick;
                event.previousPollTick = previousPoll;
                event.previousTimerEndTick = previousTimerEnd;
                event.requestBeginTick = m_lastRequestBeginTick;
                event.requestEndTick = m_lastRequestEndTick;
                event.tickerBeginTick = m_lastTickerBeginTick;
                event.tickerEndTick = m_lastTickerEndTick;
                event.sleepBeginTick = m_lastSleepBeginTick;
                event.sleepEndTick = m_lastSleepEndTick;
                event.previousCallbackMs = previousCallbackMs;
                event.longestCallbackMs = longestCallbackMs;
                event.batchIndex = batchIndex;
                event.batchCount = tasks.size();
                event.pendingTimers = m_pendingTimers.load();
                event.requestDepth = m_queueDepth.load();
                m_timerTraces.retain(event);
            }
        }

        task->run();
        if (m_timerTraceThresholdMs != 0)
        {
            previousCallbackMs = get_now_tick() - executeTick;
            longestCallbackMs = (std::max)(longestCallbackMs, previousCallbackMs);
        }
        delete task;

        ++m_timerTasks;
        --m_pendingTimers;
        --m_pendingTasks;
        ++batchIndex;
    }
    if (m_timerTraceThresholdMs != 0)
        m_lastTimerEndTick = get_now_tick();
}

void Worker::run_task()
{
    if (m_localRequests == nullptr)
        m_localRequests = m_requests.pop_all();

    for (size_t count = 0; m_localRequests != nullptr && count < REQUEST_BATCH_LIMIT; ++count)
    {
        auto task = m_localRequests;
        m_localRequests = task->m_next;

        if (m_diagnosticsEnabled)
        {
            --m_queueDepth;
            update_max(m_maxQueueDelayMs, static_cast<uint64_t>(get_now_tick() - task->m_submittedTick));
        }

        if (task->m_expireTick >= 0)
        {
            if (m_timerTraceThresholdMs != 0)
                task->m_timerInsertedTick = get_now_tick();
            m_timer.push_at(task, task->m_expireTick);
        }
        else
        {
            task->run();
            delete task;
            ++m_immediateTasks;
            --m_pendingTasks;
        }
    }
}

WorkerStatistics Worker::statistics() const
{
    return {m_immediateTasks.load(),
            m_timerTasks.load(),
            m_cancelledTasks.load(),
            m_pendingTasks.load(),
            m_pendingTimers.load(),
            m_maxQueueDepth.load(),
            m_maxQueueDelayMs.load(),
            m_maxTimerDelayMs.load(),
            m_busyNs.load(),
            m_idleNs.load(),
            m_waitCount.load(),
            m_retainedTimerBytes.load(),
            m_sleepReadySamples.load(),
            m_sleepBlockedSamples.load(),
            m_sleepPastTimerCount.load(),
            m_sleepPastTimerMs.load(),
            m_sleepOvershootNs.load(),
            m_sleepWakeRequests.load(),
            m_signalWaits.load(),
            m_notifiedWaits.load(),
            m_wakeLatencyNs.load(),
            m_maxWakeLatencyNs.load(),
            m_timerPhaseNs.load(),
            m_requestPhaseNs.load(),
            m_tickerPhaseNs.load(),
            m_blockedIdleNs.load()};
}
