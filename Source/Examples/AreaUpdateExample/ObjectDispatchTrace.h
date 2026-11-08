#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

struct ObjectDispatchStatistics
{
    uint64_t samples = 0;
    uint64_t queueNs = 0, maxQueueNs = 0;
    uint64_t actorNs = 0, maxActorNs = 0;
    uint64_t timerLateMs = 0, maxTimerLateMs = 0;
    std::array<uint64_t, 201> queueHistogram{};
    std::array<uint64_t, 201> timerHistogram{};
};

struct ObjectDispatchSample
{
    int64_t submittedNs = 0;
    int64_t deadlineTick = 0;
    bool initialUpdate = true;
    ObjectDispatchStatistics statistics{};
};

struct ObjectDispatchProfile
{
    uint64_t selectedActors = 0;
    ObjectDispatchStatistics statistics{};
    std::vector<uint64_t> workerSamples{};
};

inline bool sample_object_dispatch(uint64_t actorTag, uint64_t stride)
{
    if (stride == 0)
        return false;

    // Mix IDs so a power-of-two stride does not select just one timer Worker.
    auto mixed = actorTag + 0x9e3779b97f4a7c15ULL;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31;
    return mixed % stride == 0;
}

inline int64_t object_trace_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void record_object_dispatch(ObjectDispatchStatistics& statistics, uint64_t queueNs, uint64_t actorNs,
                                   uint64_t timerLateMs)
{
    ++statistics.samples;
    statistics.queueNs += queueNs;
    statistics.actorNs += actorNs;
    statistics.timerLateMs += timerLateMs;
    statistics.maxQueueNs = (std::max)(statistics.maxQueueNs, queueNs);
    statistics.maxActorNs = (std::max)(statistics.maxActorNs, actorNs);
    statistics.maxTimerLateMs = (std::max)(statistics.maxTimerLateMs, timerLateMs);
    ++statistics.queueHistogram[(std::min)(queueNs / 1000000, uint64_t{200})];
    ++statistics.timerHistogram[(std::min)(timerLateMs, uint64_t{200})];
}

inline void merge_object_dispatch(ObjectDispatchStatistics& destination, const ObjectDispatchStatistics& source)
{
    destination.samples += source.samples;
    destination.queueNs += source.queueNs;
    destination.actorNs += source.actorNs;
    destination.timerLateMs += source.timerLateMs;
    destination.maxQueueNs = (std::max)(destination.maxQueueNs, source.maxQueueNs);
    destination.maxActorNs = (std::max)(destination.maxActorNs, source.maxActorNs);
    destination.maxTimerLateMs = (std::max)(destination.maxTimerLateMs, source.maxTimerLateMs);
    for (size_t i = 0; i < destination.queueHistogram.size(); ++i)
    {
        destination.queueHistogram[i] += source.queueHistogram[i];
        destination.timerHistogram[i] += source.timerHistogram[i];
    }
}
