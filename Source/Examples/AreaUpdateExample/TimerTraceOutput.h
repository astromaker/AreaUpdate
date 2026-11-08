#pragma once

#include "Worker.h"
#include "TaskExecuter.h"
#include "ObjectDispatchTrace.h"
#include <cstdio>

inline uint64_t timer_delay_percentile(const std::array<uint64_t, 201>& histogram, uint64_t count, uint64_t numerator,
                                       uint64_t denominator)
{
    if (count == 0)
        return 0;
    const auto target = (count * numerator + denominator - 1) / denominator;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < histogram.size(); ++i)
    {
        cumulative += histogram[i];
        if (cumulative >= target)
            return i;
    }

    return 200;
}

inline void write_object_profile(const ObjectDispatchProfile& profile, uint64_t stride)
{
    const auto& statistics = profile.statistics;
    const auto count = statistics.samples;
    std::printf("OBJECT_PROFILE object_sample_stride=%llu object_sample_actors=%llu object_samples=%llu "
                "object_queue_mean_us=%.3f object_queue_max_us=%.3f object_queue_p50_ms=%llu "
                "object_queue_p95_ms=%llu object_queue_p99_ms=%llu object_timer_mean_ms=%.3f "
                "object_timer_max_ms=%llu object_timer_p50_ms=%llu object_timer_p95_ms=%llu "
                "object_timer_p99_ms=%llu object_actor_mean_us=%.3f object_actor_max_us=%.3f\n",
                stride, profile.selectedActors, count, count != 0 ? statistics.queueNs / 1000.0 / count : 0.0,
                statistics.maxQueueNs / 1000.0, timer_delay_percentile(statistics.queueHistogram, count, 50, 100),
                timer_delay_percentile(statistics.queueHistogram, count, 95, 100),
                timer_delay_percentile(statistics.queueHistogram, count, 99, 100),
                count != 0 ? statistics.timerLateMs / static_cast<double>(count) : 0.0, statistics.maxTimerLateMs,
                timer_delay_percentile(statistics.timerHistogram, count, 50, 100),
                timer_delay_percentile(statistics.timerHistogram, count, 95, 100),
                timer_delay_percentile(statistics.timerHistogram, count, 99, 100),
                count != 0 ? statistics.actorNs / 1000.0 / count : 0.0, statistics.maxActorNs / 1000.0);

    for (size_t id = 0; id < profile.workerSamples.size(); ++id)
        std::printf("OBJECT_SAMPLE_WORKER timer_worker=%zu samples=%llu\n", id, profile.workerSamples[id]);
}

inline void write_dispatcher_profile(const std::vector<DispatcherWorkerStatistics>& workers, uint64_t stride)
{
    uint64_t dispatchTasks = 0, executerTasks = 0, samples = 0;
    for (size_t id = 0; id < workers.size(); ++id)
    {
        const auto& statistics = workers[id];
        dispatchTasks += statistics.dispatchTasks;
        executerTasks += statistics.executerTasks;
        samples += statistics.samples;
        const auto count = statistics.samples;
        std::printf("DISPATCHER_WORKER id=%zu dispatch_tasks=%llu executer_tasks=%llu batches=%llu "
                    "max_batch_tasks=%llu busy_ms=%.3f idle_ms=%.3f elapsed_ms=%.3f waits=%llu samples=%llu "
                    "queue_mean_ms=%.3f queue_p50_ms=%llu queue_p95_ms=%llu queue_p99_ms=%llu queue_max_ms=%llu "
                    "run_mean_us=%.3f run_max_us=%.3f\n",
                    id, statistics.dispatchTasks, statistics.executerTasks, statistics.batches,
                    statistics.maxBatchTasks, statistics.busyNs / 1000000.0, statistics.idleNs / 1000000.0,
                    statistics.elapsedNs / 1000000.0, statistics.waits, count,
                    count != 0 ? statistics.queueDelayMs / static_cast<double>(count) : 0.0,
                    timer_delay_percentile(statistics.queueHistogram, count, 50, 100),
                    timer_delay_percentile(statistics.queueHistogram, count, 95, 100),
                    timer_delay_percentile(statistics.queueHistogram, count, 99, 100), statistics.maxQueueDelayMs,
                    count != 0 ? statistics.sampledRunNs / 1000.0 / count : 0.0, statistics.maxSampledRunNs / 1000.0);
    }
    std::printf("DISPATCHER_PROFILE dispatcher_profile_stride=%llu dispatcher_profile_workers=%zu "
                "dispatcher_dispatch_tasks=%llu dispatcher_executer_tasks=%llu dispatcher_samples=%llu\n",
                stride, workers.size(), dispatchTasks, executerTasks, samples);
}

inline void write_timer_profile(Worker* workerPool, size_t workerCount, uint64_t thresholdMs)
{
    std::array<uint64_t, 201> histogram{};
    uint64_t observed = 0, slow = 0, retained = 0;
    for (size_t id = 0; id < workerCount; ++id)
    {
        const auto summary = workerPool[id].timer_trace_summary();
        observed += summary.observed;
        slow += summary.slow;
        retained += summary.retained;
        for (size_t i = 0; i < histogram.size(); ++i)
            histogram[i] += summary.histogram[i];

        for (const auto& event : workerPool[id].timer_traces())
        {
            std::printf(
                "TIMER_TRACE worker=%zu actor=%llu delay_ms=%llu submitted_ms=%lld deadline_ms=%lld inserted_ms=%lld "
                "eligible_ms=%lld poll_ms=%lld selected_ms=%lld execute_ms=%lld prev_poll_ms=%lld "
                "prev_timer_end_ms=%lld request_begin_ms=%lld request_end_ms=%lld ticker_begin_ms=%lld "
                "ticker_end_ms=%lld sleep_begin_ms=%lld sleep_end_ms=%lld prev_callback_ms=%lld "
                "longest_callback_ms=%lld batch_index=%zu batch_count=%zu pending_timers=%llu request_depth=%llu\n",
                id, event.actorTag, event.delayMs, event.submittedTick, event.deadlineTick, event.insertedTick,
                event.eligibleTick, event.pollTick, event.selectedTick, event.executeTick, event.previousPollTick,
                event.previousTimerEndTick, event.requestBeginTick, event.requestEndTick, event.tickerBeginTick,
                event.tickerEndTick, event.sleepBeginTick, event.sleepEndTick, event.previousCallbackMs,
                event.longestCallbackMs, event.batchIndex, event.batchCount, event.pendingTimers, event.requestDepth);
        }
    }

    std::printf(
        "TIMER_PROFILE slot_ms=%lld slots=%zu trace_threshold_ms=%llu trace_tasks=%llu trace_slow=%llu "
        "trace_retained=%llu trace_dropped=%llu timer_p50_ms=%llu timer_p95_ms=%llu timer_p99_ms=%llu "
        "timer_p999_ms=%llu\n",
        WheelTimer::resolution_ms(), WheelTimer::slot_count(), thresholdMs, observed, slow, retained, slow - retained,
        timer_delay_percentile(histogram, observed, 50, 100), timer_delay_percentile(histogram, observed, 95, 100),
        timer_delay_percentile(histogram, observed, 99, 100), timer_delay_percentile(histogram, observed, 999, 1000));
}
