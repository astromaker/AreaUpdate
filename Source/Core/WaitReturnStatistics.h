#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

enum class WaitReturnKind : size_t
{
    Timeout,
    Signaled,
    Untimed,
    Poll
};

struct WaitReturnBucket
{
    uint64_t returns = 0, stoppingReturns = 0;
    uint64_t lateTimerReturns = 0, lateTimerMs = 0;
    uint64_t overshootNs = 0, maxOvershootNs = 0;
    uint64_t notificationSamples = 0, notificationNs = 0, maxNotificationNs = 0;
    uint64_t lateSignalRequests = 0, onTimeSignalLateReturns = 0;
    uint64_t requestedWaitNs = 0, pastDueAtBegin = 0;
    std::array<uint64_t, 101> overshootHistogram{};

    void observe(uint64_t overshoot, uint64_t timerLateMs)
    {
        ++returns;
        overshootNs += overshoot;
        maxOvershootNs = (std::max)(maxOvershootNs, overshoot);
        ++overshootHistogram[(std::min)(overshoot / 1000000, uint64_t{100})];
        if (timerLateMs != 0)
        {
            ++lateTimerReturns;
            lateTimerMs += timerLateMs;
        }
    }

    void merge(const WaitReturnBucket& other)
    {
        returns += other.returns;
        stoppingReturns += other.stoppingReturns;
        lateTimerReturns += other.lateTimerReturns;
        lateTimerMs += other.lateTimerMs;
        overshootNs += other.overshootNs;
        maxOvershootNs = (std::max)(maxOvershootNs, other.maxOvershootNs);
        notificationSamples += other.notificationSamples;
        notificationNs += other.notificationNs;
        maxNotificationNs = (std::max)(maxNotificationNs, other.maxNotificationNs);
        lateSignalRequests += other.lateSignalRequests;
        onTimeSignalLateReturns += other.onTimeSignalLateReturns;
        requestedWaitNs += other.requestedWaitNs;
        pastDueAtBegin += other.pastDueAtBegin;
        for (size_t index = 0; index < overshootHistogram.size(); ++index)
            overshootHistogram[index] += other.overshootHistogram[index];
    }

    size_t overshoot_percentile_ms(uint64_t percent) const
    {
        if (returns == 0)
            return 0;

        const auto target = (returns * percent + 99) / 100;
        uint64_t count = 0;
        for (size_t index = 0; index < overshootHistogram.size(); ++index)
        {
            count += overshootHistogram[index];
            if (count >= target)
                return index;
        }
        return overshootHistogram.size() - 1;
    }
};

struct WaitReturnStatistics
{
    bool enabled = false;
    std::array<WaitReturnBucket, 4> outcomes{};

    void merge(const WaitReturnStatistics& other)
    {
        enabled = enabled || other.enabled;
        for (size_t index = 0; index < outcomes.size(); ++index)
            outcomes[index].merge(other.outcomes[index]);
    }
};
