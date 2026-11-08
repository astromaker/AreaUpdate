#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

struct TimerDelayTrace
{
    uint64_t actorTag = 0;
    uint64_t delayMs = 0;
    int64_t submittedTick = 0;
    int64_t deadlineTick = 0;
    int64_t insertedTick = 0;
    int64_t eligibleTick = 0;
    int64_t pollTick = 0;
    int64_t selectedTick = 0;
    int64_t executeTick = 0;
    int64_t previousPollTick = 0;
    int64_t previousTimerEndTick = 0;
    int64_t requestBeginTick = 0;
    int64_t requestEndTick = 0;
    int64_t tickerBeginTick = 0;
    int64_t tickerEndTick = 0;
    int64_t sleepBeginTick = 0;
    int64_t sleepEndTick = 0;
    int64_t previousCallbackMs = 0;
    int64_t longestCallbackMs = 0;
    size_t batchIndex = 0;
    size_t batchCount = 0;
    uint64_t pendingTimers = 0;
    uint64_t requestDepth = 0;
};

struct TimerTraceSummary
{
    uint64_t observed = 0;
    uint64_t slow = 0;
    size_t retained = 0;
    std::array<uint64_t, 201> histogram{};
};

// Worker-owned bounded top-K storage; no allocation or I/O on timer execution.
class TimerTraceBuffer
{
    static constexpr size_t CAPACITY = 32;
    std::array<TimerDelayTrace, CAPACITY> m_events{};
    TimerTraceSummary m_summary{};

    static bool compare(const TimerDelayTrace& left, const TimerDelayTrace& right)
    {
        return left.delayMs > right.delayMs;
    }

public:
    void observe(uint64_t delay)
    {
        ++m_summary.observed;
        ++m_summary.histogram[(std::min)(delay, uint64_t{200})];
    }

    void retain(const TimerDelayTrace& event)
    {
        ++m_summary.slow;
        if (m_summary.retained < CAPACITY)
        {
            m_events[m_summary.retained++] = event;
            std::push_heap(m_events.begin(), m_events.begin() + m_summary.retained, compare);
        }
        else if (event.delayMs > m_events.front().delayMs)
        {
            std::pop_heap(m_events.begin(), m_events.end(), compare);
            m_events.back() = event;
            std::push_heap(m_events.begin(), m_events.end(), compare);
        }
    }

    TimerTraceSummary summary() const
    {
        return m_summary;
    }

    std::vector<TimerDelayTrace> events() const
    {
        return {m_events.begin(), m_events.begin() + m_summary.retained};
    }
};
