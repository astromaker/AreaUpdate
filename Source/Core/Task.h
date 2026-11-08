
#pragma once

#include <cstdint>
#include <chrono>
#include <atomic>

#ifndef AREAUPDATE_TASK_STATISTICS
#define AREAUPDATE_TASK_STATISTICS 1
#endif

#if AREAUPDATE_TASK_STATISTICS != 0 && AREAUPDATE_TASK_STATISTICS != 1
#error AREAUPDATE_TASK_STATISTICS must be 0 or 1
#endif

class TaskQueue;
class Worker;
class WheelTimer;
class TaskDispatcher;

struct TaskStatistics
{
    uint64_t created = 0;
    uint64_t destroyed = 0;
    uint64_t live = 0;
};

class Task
{
    friend class TaskQueue;
    friend class Worker;
    friend class WheelTimer;
    friend class TaskDispatcher;
    Task* m_next = nullptr;
    int64_t m_submittedTick = 0;
    int64_t m_expireTick = -1;
    int64_t m_wheelTick = 0;
    int64_t m_timerInsertedTick = 0;

#if AREAUPDATE_TASK_STATISTICS
    struct Counters
    {
        std::atomic_uint64_t created{0};
        std::atomic_uint64_t destroyed{0};
        std::atomic_uint64_t live{0};
    };
    static Counters& counters()
    {
        static Counters value;
        return value;
    }
#endif

public:
    Task()
    {
#if AREAUPDATE_TASK_STATISTICS
        ++counters().created;
        ++counters().live;
#endif
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    virtual ~Task()
    {
#if AREAUPDATE_TASK_STATISTICS
        ++counters().destroyed;
        --counters().live;
#endif
    }

    static constexpr bool statistics_enabled()
    {
        return AREAUPDATE_TASK_STATISTICS != 0;
    }

    static TaskStatistics statistics()
    {
#if AREAUPDATE_TASK_STATISTICS
        auto& value = counters();
        return {value.created.load(), value.destroyed.load(), value.live.load()};
#else
        return {};
#endif
    }

    virtual void run() = 0;
    virtual uint64_t trace_tag() const
    {
        return 0;
    }
};

inline int64_t get_now_tick()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
