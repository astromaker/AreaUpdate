
#include <cstdio>
#include <atomic>
#include <limits>
#include <memory>
#include <random>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <algorithm>

#include "Worker.h"
#include "AreaExecuter.h"
#include "AreaManager.h"
#include "RuntimeBackend.h"
#include "TimerTraceOutput.h"
#include "RadiusLatencyOutput.h"

constexpr size_t THREAD_COUNT = 8;
constexpr size_t PARTITION_RADIUS = 2;
constexpr size_t OBJECT_COUNT = 70000;
constexpr size_t MOVE_COUNT_PER_SEC = 200;
constexpr int64_t JOIN_DURATION_MS = 5000;
constexpr int64_t JOIN_INTERVAL_MS = 1;
constexpr int64_t FPS = 20;

std::atomic_int32_t maxTickCount{0};
std::atomic_int32_t minTickCount{(std::numeric_limits<int32_t>::max)()};

AreaExecuter executor(PARTITION_RADIUS);
Worker workers[THREAD_COUNT];
volatile std::sig_atomic_t stopRequested = 0;

class MyArea : public Area
{
    int32_t m_updateCount = 0;
    int64_t m_updateTick = 0;

public:
    MyArea(int32_t x, int32_t y)
        : Area(x, y)
        , m_updateTick(get_now_tick())
    {
    }

private:
    void do_update() override
    {
        ++m_updateCount;

        auto nowTick = get_now_tick();
        auto runTick = nowTick - m_updateTick;

        if (runTick >= 1000)
        {
            if (maxTickCount < m_updateCount)
                maxTickCount = m_updateCount;
            if (minTickCount > m_updateCount)
                minTickCount = m_updateCount;

            m_updateCount = 0;
            m_updateTick = nowTick;
        }
    }
};

struct TestObject : public Actor
{
    int32_t x = 0;
    int32_t y = 0;
    MyArea* area = nullptr;
    int64_t joinedTick = 0;
    std::atomic_uint64_t updateCount{0};
    std::atomic_bool updating{false};
    std::atomic_uint64_t overlaps{0};
    int64_t lastUpdate = 0;
    uint64_t maxGap = 0;
    uint64_t gapHistogram[101] = {};
    uint64_t lateGaps = 0;
    uint64_t lateGapMs = 0;
    size_t workIterations = 0;
    uint64_t workValue = 1;

    void update() override
    {
        if (updating.exchange(true))
        {
            ++overlaps;
            return;
        }

        const auto now = get_now_tick();
        if (lastUpdate != 0)
        {
            const auto gap = static_cast<uint64_t>(now - lastUpdate);
            if (gap > maxGap)
                maxGap = gap;
            ++gapHistogram[(std::min)(gap / 10, uint64_t{100})];
            if (gap > 50)
            {
                ++lateGaps;
                lateGapMs += gap - 50;
            }
        }

        auto value = workValue;
        for (size_t i = 0; i < workIterations; ++i)
        {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
        }
        workValue = value;

        lastUpdate = now;
        ++updateCount;
        updating.store(false);
    }
};

void write_wait_returns(const WaitReturnStatistics& stats, const char* scope, int32_t worker)
{
    if (stats.enabled == false)
        return;

    const std::array<const char*, 4> names{{"timeout", "signal", "untimed", "poll"}};
    for (size_t index = 0; index < names.size(); ++index)
    {
        const auto& bucket = stats.outcomes[index];
        std::printf("WAIT_RETURN scope=%s worker=%d kind=%s returns=%llu stopping=%llu late_timer_returns=%llu "
                    "late_timer_ms=%llu overshoot_ns=%llu overshoot_p99_bucket_ms=%zu overshoot_max_ns=%llu "
                    "notification_samples=%llu notification_ns=%llu notification_max_ns=%llu "
                    "late_signal_requests=%llu on_time_signal_late_returns=%llu "
                    "requested_wait_ns=%llu past_due_at_begin=%llu\n",
                    scope, worker, names[index], bucket.returns, bucket.stoppingReturns, bucket.lateTimerReturns,
                    bucket.lateTimerMs, bucket.overshootNs, bucket.overshoot_percentile_ms(99), bucket.maxOvershootNs,
                    bucket.notificationSamples, bucket.notificationNs, bucket.maxNotificationNs,
                    bucket.lateSignalRequests, bucket.onTimeSignalLateReturns, bucket.requestedWaitNs,
                    bucket.pastDueAtBegin);
    }
}

void write_area_profile(size_t stride, int64_t startOffsetMs)
{
    std::printf("AREA_DIAGNOSTICS area_profile_stride=%zu area_profile_start_ms=%lld\n", stride, startOffsetMs);
    if (stride == 0)
        return;

    const auto rows = executor.profile_seconds();
    for (size_t second = 0; second < rows.size(); ++second)
    {
        const auto& row = rows[second];
        if (row.batchNs.count == 0 && row.emptyBatches == 0 && row.blockedReturnNs.count == 0)
            continue;

        std::printf("AREA_PROFILE_SECOND second=%zu empty_batches=%llu budget_exhausted_epochs=%llu "
                    "budget_backlog_epochs=%llu epoch_samples_dropped=%llu\n",
                    second, row.emptyBatches, row.budgetExhaustedEpochs, row.budgetBacklogEpochs,
                    row.epochSamplesDropped);
        auto write = [second](const char* metric, const AreaProfileDistribution& value)
        {
            std::printf("AREA_PROFILE second=%zu metric=%s count=%llu sum=%llu max=%llu p50_upper=%llu "
                        "p95_upper=%llu p99_upper=%llu\n",
                        second, metric, value.count, value.sum, value.maximum, value.percentile_upper(50),
                        value.percentile_upper(95), value.percentile_upper(99));
        };
        write("batch_ns", row.batchNs);
        write("batch_areas", row.batchAreas);
        write("batch_actor_reservations", row.batchActorReservations);
        write("timer_late_ns", row.timerLateNs);
        write("ready_to_run_ns", row.readyToRunNs);
        write("enqueue_ns", row.enqueueNs);
        write("enqueue_profile_lock_ns", row.enqueueProfileLockNs);
        write("queue_residence_ns", row.queueResidenceNs);
        write("extracted_to_run_ns", row.extractedToRunNs);
        write("admission_lock_ns", row.admissionLockNs);
        write("deadline_to_run_ns", row.deadlineToRunNs);
        write("epoch_tail_ns", row.epochTailNs);
        write("blocked_return_ns", row.blockedReturnNs);
        write("epoch_areas", row.epochAreas);
        write("epoch_admissions", row.epochAdmissions);
        write("epoch_remaining_areas", row.epochRemainingAreas);
        write("epoch_ns", row.epochNs);
        write("backlog_epoch_areas", row.backlogEpochAreas);
        write("backlog_epoch_ns", row.backlogEpochNs);
    }

    const auto partitions = executor.profile_partitions();
    for (size_t id = 0; id < partitions.size(); ++id)
    {
        const auto& value = partitions[id];
        std::printf("AREA_PROFILE_PARTITION id=%zu epochs=%llu admissions=%llu areas=%llu "
                    "budget_exhausted_epochs=%llu budget_backlog_epochs=%llu remaining_areas_sum=%llu "
                    "max_remaining_areas=%zu duration_ns=%llu\n",
                    id, value.epochs, value.admissions, value.areas, value.budgetExhaustedEpochs,
                    value.budgetBacklogEpochs, value.remainingAreasSum, value.maxRemainingAreas, value.durationNs);
    }
    for (const auto& value : executor.profile_epochs())
    {
        std::printf("AREA_PROFILE_EPOCH sequence=%llu partition=%zu elapsed_ns=%lld duration_ns=%llu "
                    "admissions=%zu areas=%zu remaining_areas=%zu budget_exhausted=%d\n",
                    value.sequence, value.partition, value.elapsedNs, value.durationNs, value.admissions, value.areas,
                    value.remainingAreas, static_cast<int>(value.budgetExhausted));
    }
}

int main(int argc, char** argv)
{
    const auto programStart = get_now_tick();
    std::signal(SIGINT,
                [](int)
                {
                    stopRequested = 1;
                });

    const auto durationSeconds = argc > 1 ? std::strtoll(argv[1], nullptr, 10) : int64_t{0};
    const size_t objectCount = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : OBJECT_COUNT;
    const size_t movesPerSecond = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : MOVE_COUNT_PER_SEC;
    const uint32_t seed = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 42;
    const auto joinDurationMs = argc > 5 ? std::strtoll(argv[5], nullptr, 10) : JOIN_DURATION_MS;
    const auto joinIntervalMs = argc > 6 ? std::strtoll(argv[6], nullptr, 10) : JOIN_INTERVAL_MS;
    const size_t actorWork = argc > 7 ? std::strtoull(argv[7], nullptr, 10) : size_t{0};
    const auto mapSide = argc > 8 ? std::strtoll(argv[8], nullptr, 10) : int64_t{0};
    const std::string mode = argc > 9 ? argv[9] : "area";
    const uint64_t traceThresholdMs = argc > 10 ? std::strtoull(argv[10], nullptr, 10) : uint64_t{0};
    const std::string waitMode = argc > 11 ? argv[11] : "signal";
    const uint64_t objectSampleStride = argc > 12 ? std::strtoull(argv[12], nullptr, 10) : uint64_t{0};
    const uint64_t dispatcherProfileStride = argc > 13 ? std::strtoull(argv[13], nullptr, 10) : uint64_t{0};
    const size_t areaProfileStride = argc > 14 ? std::strtoull(argv[14], nullptr, 10) : size_t{0};
    const std::string admissionMode = argc > 15 ? argv[15] : "unlimited";
    if (admissionMode != "adaptive" && admissionMode != "fixed" && admissionMode != "unlimited")
        return 2;

    AreaSchedulingOptions scheduling{};
    scheduling.adaptive = admissionMode == "adaptive";
    scheduling.unlimited = admissionMode == "unlimited";
    scheduling.maxSliceMs = argc > 16 ? std::strtoll(argv[16], nullptr, 10) : scheduling.maxSliceMs;
    scheduling.maxOtherWaitMs = argc > 17 ? std::strtoll(argv[17], nullptr, 10) : scheduling.maxOtherWaitMs;
    scheduling.maxAdmissions = argc > 18 ? std::strtoull(argv[18], nullptr, 10) : scheduling.maxAdmissions;
    const std::string layout = argc > 19 ? argv[19] : "uniform";
    if (scheduling.maxSliceMs <= 0 || scheduling.maxOtherWaitMs <= 0 || scheduling.maxAdmissions < 8
        || scheduling.maxAdmissions > 1024
        || (layout != "uniform" && layout != "clustered" && layout != "hotspot" && layout != "partition-skew"
            && layout != "boundary"))
        return 2;
    executor.configure_scheduling(scheduling);
    if (objectCount == 0 || joinDurationMs < 0 || joinIntervalMs <= 0 || mapSide < 0 || mapSide > 2048
        || traceThresholdMs > 60000 || objectSampleStride > 1000000 || dispatcherProfileStride > 1000000
        || (argc > 1 && durationSeconds <= 0))
        return 2;
    if (mode != "area" && mode != "original" && mode != "job" && mode != "legacy")
        return 2;
    if (waitMode != "signal" && waitMode != "poll")
        return 2;
    if ((objectSampleStride != 0 || dispatcherProfileStride != 0) && mode != "job")
        return 2;
    if (areaProfileStride > 1000000
        || (areaProfileStride != 0 && (mode != "area" || durationSeconds <= 0 || durationSeconds > 3600)))
        return 2;

    std::unique_ptr<RadiusLatencyProfile> radiusLatency{};
    RuntimeBackend<MyArea> backend(mode, executor, workers, THREAD_COUNT, PARTITION_RADIUS, objectSampleStride,
                                   dispatcherProfileStride);

#ifdef _DEBUG
    float minX = 0;
    float minY = 0;
    float maxX = 400;
    float maxY = 400;
#else
    float minX = 0;
    float minY = 0;
    float maxX = 16 * 1000;
    float maxY = 16 * 1000;
#endif
    int32_t areaX = 50;
    int32_t areaY = 50;
    if (mapSide > 0)
    {
        maxX = static_cast<float>(mapSide * areaX);
        maxY = static_cast<float>(mapSide * areaY);
    }

    AreaManager<MyArea> manager;
    manager.build(minX, minY, maxX, maxY, areaX, areaY);
    if (layout == "boundary" && (manager.get_count_x() < 2 || manager.get_count_y() < 2))
        return 2;

    if (mode == "area")
    {
        manager.work(
            [](int32_t, int32_t, MyArea* area)
            {
                area->set_wake(
                    [](Area* value)
                    {
                        executor.push(value);
                    });
            });
    }
    backend.prepare(manager);
    const auto latencyText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_LATENCY_STRIDE");
    const size_t latencyStride = latencyText.empty() ? size_t{0} : std::stoull(latencyText);
    if (latencyStride > 1000000 || (latencyStride != 0 && mode != "legacy"))
        return 2;
    if (latencyStride != 0)
    {
        radiusLatency = std::make_unique<RadiusLatencyProfile>(manager.get_count_x(), manager.get_count_y());
        backend.configure_radius_trace(
            [&](MyArea* area, const RadiusRuntime::TraceEvent& event)
            {
                radiusLatency->observe(area->get_x(), area->get_y(), event);
            },
            latencyStride);
    }
    const auto profileStart = get_now_tick();
    const auto diagnosticsText = RadiusRuntime::environment_value("AREAUPDATE_WORKER_DIAGNOSTICS");
    if (diagnosticsText.empty() == false && diagnosticsText != "0" && diagnosticsText != "1")
        throw std::invalid_argument("Worker diagnostics must be 0 or 1");
    const bool workerDiagnostics = diagnosticsText.empty() || diagnosticsText == "1";
    const auto waitReturnText = RadiusRuntime::environment_value("AREAUPDATE_WAIT_RETURN_DIAGNOSTICS");
    if (waitReturnText.empty() == false && waitReturnText != "0" && waitReturnText != "1")
        throw std::invalid_argument("Wait return diagnostics must be 0 or 1");
    const bool waitReturnDiagnostics = waitReturnText == "1";
    std::printf("RUNTIME_DIAGNOSTICS worker_diagnostics_enabled=%d\n", static_cast<int>(workerDiagnostics));
    executor.configure_profile(areaProfileStride, static_cast<size_t>(durationSeconds), profileStart);
    for (size_t i = 0; i < THREAD_COUNT; ++i)
    {
        workers[i].set_runtime_diagnostics(workerDiagnostics);
        workers[i].set_wait_return_diagnostics(waitReturnDiagnostics);
        workers[i].set_timer_trace_threshold(traceThresholdMs);
        workers[i].set_polling_wait(waitMode == "poll");
        workers[i].start(backend.ticker(), i);
    }

    size_t areaCount = 0;
    std::mt19937 engine{seed};
    const auto measureStart = get_now_tick();
    const auto endTime =
        durationSeconds > 0 ? measureStart + durationSeconds * 1000 : (std::numeric_limits<int64_t>::max)();
    uint64_t successfulMoves = 0;

    std::vector<std::unique_ptr<TestObject>> objects;
    objects.reserve(objectCount);

    std::uniform_int_distribution<int32_t> areaXDist(0, manager.get_count_x() - 1);
    std::uniform_int_distribution<int32_t> areaYDist(0, manager.get_count_y() - 1);

    auto addObject = [&]
    {
        int32_t x = areaXDist(engine);
        int32_t y = areaYDist(engine);
        if (layout == "hotspot")
        {
            x = manager.get_count_x() / 2;
            y = manager.get_count_y() / 2;
        }
        else if (layout == "boundary")
        {
            x = manager.get_count_x() / 2 - 1 + x % 2;
            y = manager.get_count_y() / 2 - 1 + y % 2;
        }
        else if (layout == "clustered")
        {
            x = (std::max)(0, (std::min)(manager.get_count_x() / 2 + x % 5 - 2, manager.get_count_x() - 1));
            y = (std::max)(0, (std::min)(manager.get_count_y() / 2 + y % 5 - 2, manager.get_count_y() - 1));
        }
        else if (layout == "partition-skew" && objects.size() % 10 != 9)
        {
            const int32_t width = static_cast<int32_t>(2 * PARTITION_RADIUS + 1);
            x -= x % width;
            y -= y % width;
        }
        MyArea* area = manager.get_area(x, y);
        if (area == nullptr)
            return false;

        auto object = std::make_unique<TestObject>();
        object->x = x;
        object->y = y;
        object->area = area;
        object->joinedTick = get_now_tick();
        object->workIterations = actorWork;
        object->workValue = seed + objects.size() + 1;

        backend.add_object(object.get(), area);
        objects.emplace_back(std::move(object));
        return true;
    };

    auto starter = [&areaCount](int32_t, int32_t, MyArea*)
    {
        ++areaCount;
    };
    manager.work(starter);

    std::uniform_int_distribution<int32_t> moveDist(-1, 1);
    auto nextMoveTick = measureStart + 1000;
    auto nextReportTick = measureStart + 1000;
    auto nextJoinTick = measureStart;
    size_t joinSteps = 0, maxJoinBatch = 0;
    int64_t joinedAllMs = 0;
    uint64_t previousActorUpdates = 0, previousSkippedTicks = 0;
    double previousActorSeconds = 0;
    int64_t previousProfileTick = measureStart;
    int64_t steadyStart = 0;
    uint64_t steadyInitialUpdates = 0;
    std::vector<uint64_t> steadyActorUpdates(objectCount, 0);

    while (stopRequested == 0 && get_now_tick() < endTime)
    {
        const auto now = get_now_tick();
        if (objects.size() < objectCount && now >= nextJoinTick)
        {
            const auto elapsedMs = now - measureStart;
            const size_t joinTarget =
                joinDurationMs == 0 || elapsedMs >= joinDurationMs
                    ? objectCount
                    : 1 + static_cast<size_t>(static_cast<long double>(objectCount - 1) * elapsedMs / joinDurationMs);
            const auto previousCount = objects.size();
            while (objects.size() < joinTarget)
            {
                if (addObject() == false)
                    break;
            }

            ++joinSteps;
            maxJoinBatch = (std::max)(maxJoinBatch, objects.size() - previousCount);
            nextJoinTick = now + joinIntervalMs;
            if (objects.size() == objectCount)
                joinedAllMs = get_now_tick() - programStart;
        }

        if (steadyStart == 0 && objects.size() == objectCount && now - programStart >= joinedAllMs + 1000)
        {
            for (size_t index = 0; index < objects.size(); ++index)
            {
                const auto updates = objects[index]->updateCount.load(std::memory_order_relaxed);
                steadyActorUpdates[index] = updates;
                steadyInitialUpdates += updates;
            }
            steadyStart = get_now_tick();
        }

        const bool moveDue = now >= nextMoveTick;
        if (moveDue)
            nextMoveTick = now + 1000;

        std::uniform_int_distribution<size_t> objectDist(0, objects.empty() ? 0 : objects.size() - 1);
        for (size_t i = 0; moveDue && i < movesPerSecond && objects.empty() == false; ++i)
        {
            TestObject& object = *objects[objectDist(engine)];
            int32_t nextX = object.x + moveDist(engine);
            int32_t nextY = object.y + moveDist(engine);

            if (nextX < 0)
            {
                nextX = 0;
            }
            else if (nextX >= manager.get_count_x())
            {
                nextX = manager.get_count_x() - 1;
            }

            if (nextY < 0)
            {
                nextY = 0;
            }
            else if (nextY >= manager.get_count_y())
            {
                nextY = manager.get_count_y() - 1;
            }

            if (layout == "boundary")
            {
                nextX = (std::max)(manager.get_count_x() / 2 - 1, (std::min)(manager.get_count_x() / 2, nextX));
                nextY = (std::max)(manager.get_count_y() / 2 - 1, (std::min)(manager.get_count_y() / 2, nextY));
            }

            if (nextX == object.x && nextY == object.y)
                continue;

            MyArea* nextArea = manager.get_area(nextX, nextY);
            if (nextArea == nullptr)
                continue;

            if (backend.move_object(&object, object.area, nextArea) == false)
                continue;

            ++successfulMoves;

            object.x = nextX;
            object.y = nextY;
            object.area = nextArea;
        }

        if (now < nextReportTick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        nextReportTick = now + 1000;

        if (areaProfileStride != 0)
        {
            uint64_t updates = 0;
            double exposure = 0;
            const auto reportTick = get_now_tick();
            for (const auto& object : objects)
            {
                updates += object->updateCount.load(std::memory_order_relaxed);
                exposure += (reportTick - object->joinedTick) / 1000.0;
            }
            size_t active = 0;
            manager.work(
                [&active](int32_t, int32_t, MyArea* area)
                {
                    if (area->is_active())
                        ++active;
                });
            const auto skipped = executor.skipped_ticks();
            const auto actorSeconds = exposure - previousActorSeconds;
            std::printf("AREA_SECOND elapsed_ms=%lld interval_ms=%lld actors=%zu active_areas=%zu "
                        "updates=%llu interval_fps=%.6f skipped_ticks=%llu\n",
                        reportTick - profileStart, reportTick - previousProfileTick, objects.size(), active,
                        updates - previousActorUpdates,
                        actorSeconds > 0 ? (updates - previousActorUpdates) / actorSeconds : 0.0,
                        skipped - previousSkippedTicks);
            previousActorUpdates = updates;
            previousActorSeconds = exposure;
            previousSkippedTicks = skipped;
            previousProfileTick = reportTick;
        }

        auto minTick = minTickCount.load();
#if AREAUPDATE_RUN_COUNTER
        auto runCount = areaRunCount.load();
        printf("areaCount: %llu, objectCount: %llu, moveCount: %llu, FPS: %lld, tick: (average: %llu, min: %d, max: "
               "%d), runCount: %llu\n",
               areaCount, objects.size(), movesPerSecond, FPS, runCount / areaCount,
               minTick == (std::numeric_limits<int32_t>::max)() ? 0 : minTick, maxTickCount.load(), runCount);

        areaRunCount = 0;
#else
        printf("areaCount: %llu, objectCount: %llu, moveCount: %llu, FPS: %lld, "
               "tick: (min: %d, max: %d), runCount: disabled\n",
               areaCount, objects.size(), movesPerSecond, FPS,
               minTick == (std::numeric_limits<int32_t>::max)() ? 0 : minTick, maxTickCount.load());
#endif
        maxTickCount = 0;
        minTickCount = (std::numeric_limits<int32_t>::max)();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto measureEnd = get_now_tick();
    backend.stop();

    for (auto& worker : workers)
        worker.stop();

    for (auto& worker : workers)
        worker.join();
    backend.finish();

    const double steadySeconds = steadyStart != 0 ? (measureEnd - steadyStart) / 1000.0 : 0.0;
    const auto steadyExpected = steadyStart != 0 ? static_cast<uint64_t>((measureEnd - steadyStart) / 50) : 0;
    uint64_t steadyMinimum = (std::numeric_limits<uint64_t>::max)(), steadyMaximum = 0;
    uint64_t steadyBelowTarget = 0, steadyUnder19 = 0, steadyUnder10 = 0;
    size_t actorIndex = 0;
    uint64_t actorUpdates = 0, overlaps = 0, neverUpdated = 0, maxGap = 0;
    uint64_t histogram[101] = {};
    double actorSeconds = 0;
    uint64_t expectedUpdates = 0, updateDeficit = 0, belowTargetActors = 0;
    uint64_t lateGaps = 0, lateGapMs = 0, workChecksum = 0;
    for (const auto& object : objects)
    {
        const auto expected = static_cast<uint64_t>((measureEnd - object->joinedTick) / 50);
        const auto actual = object->updateCount.load();
        if (steadyStart != 0)
        {
            const auto updates = actual - steadyActorUpdates[actorIndex];
            steadyMinimum = (std::min)(steadyMinimum, updates);
            steadyMaximum = (std::max)(steadyMaximum, updates);
            steadyBelowTarget += steadyExpected > updates + 1;
            steadyUnder19 += updates < steadySeconds * 19.0;
            steadyUnder10 += updates < steadySeconds * 10.0;
        }
        ++actorIndex;

        expectedUpdates += expected;
        if (expected > actual)
            updateDeficit += expected - actual;
        if (expected > actual + 1)
            ++belowTargetActors;
        lateGaps += object->lateGaps;
        lateGapMs += object->lateGapMs;
        workChecksum ^= object->workValue;
        actorSeconds += (measureEnd - object->joinedTick) / 1000.0;
        actorUpdates += object->updateCount.load();
        overlaps += object->overlaps.load();
        neverUpdated += object->updateCount == 0;
        maxGap = (std::max)(maxGap, object->maxGap);
        for (size_t i = 0; i <= 100; ++i)
            histogram[i] += object->gapHistogram[i];
    }

    const double elapsed = (measureEnd - measureStart) / 1000.0;
    std::printf("STEADY_FAIRNESS steady_fairness_available=%d steady_min_actor_fps=%.6f "
                "steady_max_actor_fps=%.6f steady_below_target_actors=%llu steady_under_19_actors=%llu "
                "steady_under_10_actors=%llu\n",
                static_cast<int>(steadyStart != 0), steadySeconds > 0 ? steadyMinimum / steadySeconds : 0.0,
                steadySeconds > 0 ? steadyMaximum / steadySeconds : 0.0, steadyBelowTarget, steadyUnder19,
                steadyUnder10);
    std::printf("STEADY steady_actor_fps=%.6f steady_seconds=%.3f layout=%s admission_slice_ms=%lld "
                "admission_wait_ms=%lld admission_max=%zu\n",
                steadySeconds > 0 ? (actorUpdates - steadyInitialUpdates) / (objects.size() * steadySeconds) : 0.0,
                steadySeconds, layout.c_str(), scheduling.maxSliceMs, scheduling.maxOtherWaitMs,
                scheduling.maxAdmissions);
    const uint64_t samples = actorUpdates - (objects.size() - neverUpdated);
    uint64_t cumulative = 0, p99Bucket = 100;

    for (size_t i = 0; i <= 100; ++i)
    {
        cumulative += histogram[i];
        if (samples != 0 && cumulative >= (samples * 99 + 99) / 100)
        {
            p99Bucket = i;
            break;
        }
    }

    std::printf("SUMMARY seconds=%.3f actors=%zu seed=%u moves=%llu updates=%llu actor_fps=%.3f overlaps=%llu "
                "never_updated=%llu max_gap_ms=%llu p99_gap_bucket_ms=%llu target_actors=%zu join_duration_ms=%lld "
                "actor_seconds=%.3f join_interval_ms=%lld join_steps=%zu max_join_batch=%zu\n",
                elapsed, objects.size(), seed, successfulMoves, actorUpdates,
                actorSeconds > 0 ? actorUpdates / actorSeconds : 0.0, overlaps, neverUpdated, maxGap, p99Bucket * 10,
                objectCount, joinDurationMs, actorSeconds, joinIntervalMs, joinSteps, maxJoinBatch);
    std::printf("TARGET actor_work=%zu expected_updates=%llu update_deficit=%llu below_target_actors=%llu "
                "late_gaps=%llu late_gap_ms=%llu work_checksum=%llu map_side=%d actor_bytes=%zu area_bytes=%zu "
                "joined_all_ms=%lld mode=%s timer_workers=%zu dispatcher_workers=%zu\n",
                actorWork, expectedUpdates, updateDeficit, belowTargetActors, lateGaps, lateGapMs, workChecksum,
                manager.get_count_x(), sizeof(TestObject), sizeof(MyArea), joinedAllMs, mode.c_str(), THREAD_COUNT,
                mode == "job" ? THREAD_COUNT : size_t{0});

    uint64_t executedTasks = 0, cancelledTasks = 0, pendingTasks = 0;
    uint64_t maxQueueDelay = 0, maxTimerDelay = 0, busyNs = 0, idleNs = 0;
    uint64_t timerRetainedBytes = 0;
    uint64_t sleepCount = 0, sleepReady = 0, sleepBlocked = 0, sleepPastTimer = 0;
    uint64_t sleepPastTimerMs = 0, sleepOvershootNs = 0, sleepWakeRequests = 0;
    uint64_t signalWaits = 0, notifiedWaits = 0, wakeLatencyNs = 0, maxWakeLatencyNs = 0;
    WaitReturnStatistics waitReturns{};

    for (const auto& worker : workers)
    {
        const auto stats = worker.statistics();
        const auto returns = worker.wait_return_statistics();
        waitReturns.merge(returns);
        write_wait_returns(returns, "worker", static_cast<int32_t>(worker.get_id()));
        if (traceThresholdMs != 0)
        {
            std::printf("WORKER_PHASE id=%zu timer_ns=%llu request_ns=%llu ticker_ns=%llu blocked_idle_ns=%llu "
                        "notified_waits=%llu wake_latency_ns=%llu idle_ns=%llu busy_ns=%llu\n",
                        worker.get_id(), stats.timerPhaseNs, stats.requestPhaseNs, stats.tickerPhaseNs,
                        stats.blockedIdleNs, stats.notifiedWaits, stats.wakeLatencyNs, stats.idleNs, stats.busyNs);
        }
        executedTasks += stats.immediateTasks + stats.timerTasks;
        cancelledTasks += stats.cancelledTasks;
        pendingTasks += stats.pendingTasks;
        maxQueueDelay = (std::max)(maxQueueDelay, stats.maxQueueDelayMs);
        maxTimerDelay = (std::max)(maxTimerDelay, stats.maxTimerDelayMs);
        busyNs += stats.busyNs;
        idleNs += stats.idleNs;
        timerRetainedBytes += stats.retainedTimerBytes;
        sleepCount += stats.waitCount;
        sleepReady += stats.sleepReadySamples;
        sleepBlocked += stats.sleepBlockedSamples;
        sleepPastTimer += stats.sleepPastTimerCount;
        sleepPastTimerMs += stats.sleepPastTimerMs;
        sleepOvershootNs += stats.sleepOvershootNs;
        sleepWakeRequests += stats.sleepWakeRequests;
        signalWaits += stats.signalWaits;
        notifiedWaits += stats.notifiedWaits;
        wakeLatencyNs += stats.wakeLatencyNs;
        maxWakeLatencyNs = (std::max)(maxWakeLatencyNs, stats.maxWakeLatencyNs);
        std::printf(
            "WORKER id=%zu immediate=%llu timers=%llu cancelled=%llu pending=%llu pending_timers=%llu "
            "max_queue_depth=%llu queue_delay_ms=%llu timer_delay_ms=%llu busy_ms=%.3f idle_ms=%.3f waits=%llu\n",
            worker.get_id(), stats.immediateTasks, stats.timerTasks, stats.cancelledTasks, stats.pendingTasks,
            stats.pendingTimers, stats.maxQueueDepth, stats.maxQueueDelayMs, stats.maxTimerDelayMs,
            stats.busyNs / 1000000.0, stats.idleNs / 1000000.0, stats.waitCount);
    }

    size_t actorSlots = 0;
    std::printf("WAIT_RETURN_CONFIG wait_return_enabled=%d\n", static_cast<int>(waitReturns.enabled));
    write_wait_returns(waitReturns, "total", -1);
    size_t activeAreas = 0;
    std::vector<size_t> activePartitions((2 * PARTITION_RADIUS + 1) * (2 * PARTITION_RADIUS + 1), 0);
    manager.work(
        [&](int32_t, int32_t, MyArea* area)
        {
            actorSlots += area->retained_actor_slots();
            if (area->is_active())
            {
                ++activeAreas;
                ++activePartitions[area->get_partition(2 * PARTITION_RADIUS + 1)];
            }
        });
    std::printf("WORLD area_count=%zu active_areas=%zu partition_count=%zu min_active_per_partition=%zu "
                "max_active_per_partition=%zu\n",
                areaCount, activeAreas, activePartitions.size(),
                *std::min_element(activePartitions.begin(), activePartitions.end()),
                *std::max_element(activePartitions.begin(), activePartitions.end()));

    const auto partitionAreas = executor.partition_areas();
    for (size_t id = 0; id < partitionAreas.size(); ++id)
        std::printf("PARTITION id=%zu areas_popped=%llu\n", id, partitionAreas[id]);

    const auto taskMemory = Task::statistics();
    std::printf("DIAGNOSTICS task_statistics=%d area_run_counter=%d\n", static_cast<int>(Task::statistics_enabled()),
                AREAUPDATE_RUN_COUNTER);
    std::printf("BACKEND legacy_updates=%llu legacy_neighbor_failures=%llu object_skipped_ticks=%llu "
                "object_schedule=%s area_schedule=%s area_skipped_ticks=%llu\n",
                backend.legacy_updates(), backend.legacy_failures(), backend.object_skipped_ticks(),
                mode == "job" ? "fixed" : "not_applicable",
                mode == "area" || mode == "legacy" ? "fixed" : (mode == "original" ? "relative" : "not_applicable"),
                executor.skipped_ticks());
    std::printf("RADIUS radius_updates=%llu radius_admission_failures=%llu radius_skipped_ticks=%llu\n",
                backend.radius_updates(), backend.radius_failures(), backend.radius_skipped_ticks());
    backend.print_radius_profile();
    if (radiusLatency != nullptr)
        radiusLatency->print_report();
    std::printf("SLEEP count=%llu ready_samples=%llu blocked_samples=%llu past_timer_count=%llu "
                "past_timer_ms=%llu overshoot_ms=%.3f wake_requests=%llu\n",
                sleepCount, sleepReady, sleepBlocked, sleepPastTimer, sleepPastTimerMs, sleepOvershootNs / 1000000.0,
                sleepWakeRequests);
    std::printf("WAIT wait_mode=%s signal_waits=%llu notified_waits=%llu wake_latency_mean_us=%.3f "
                "wake_latency_max_us=%.3f\n",
                waitMode.c_str(), signalWaits, notifiedWaits,
                notifiedWaits != 0 ? wakeLatencyNs / 1000.0 / notifiedWaits : 0.0, maxWakeLatencyNs / 1000.0);
    std::printf(
        "RUNTIME tasks_executed=%llu tasks_cancelled=%llu tasks_pending=%llu tasks_created=%llu tasks_destroyed=%llu "
        "tasks_live=%llu actor_capacity_bytes=%zu partition_runs=%llu partition_changes=%llu max_queue_delay_ms=%llu "
        "max_timer_delay_ms=%llu busy_ms=%.3f idle_ms=%.3f timer_retained_bytes=%llu\n",
        executedTasks, cancelledTasks, pendingTasks, taskMemory.created, taskMemory.destroyed, taskMemory.live,
        actorSlots * sizeof(Actor*), executor.partition_runs(), executor.partition_changes(), maxQueueDelay,
        maxTimerDelay, busyNs / 1000000.0, idleNs / 1000000.0, timerRetainedBytes);

    write_timer_profile(workers, THREAD_COUNT, traceThresholdMs);
    write_object_profile(backend.dispatch_profile(), objectSampleStride);
    write_dispatcher_profile(backend.dispatcher_statistics(), dispatcherProfileStride);
    write_area_profile(areaProfileStride, profileStart - programStart);
    const auto schedulingStats = executor.scheduling_statistics();
    std::printf("AREA_SCHEDULING area_admission_mode=%s extensions=%llu slice_stops=%llu wait_stops=%llu "
                "service_wait_tracking=%d\n",
                admissionMode.c_str(), schedulingStats.extensions, schedulingStats.sliceStops,
                schedulingStats.waitStops, static_cast<int>(scheduling.adaptive || scheduling.unlimited));
    for (size_t id = 0; id < schedulingStats.maxServiceWaitMs.size(); ++id)
    {
        std::printf("AREA_SCHEDULING_PARTITION id=%zu max_service_wait_ms=%llu\n", id,
                    schedulingStats.maxServiceWaitMs[id]);
    }

    if (pendingTasks != 0 || taskMemory.live != 0)
        return 4;
    if (overlaps != 0 || neverUpdated != 0)
        return 3;

    return 0;
}
