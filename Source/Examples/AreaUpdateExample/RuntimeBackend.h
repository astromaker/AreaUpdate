#pragma once

#include "AreaExecuter.h"
#include "AreaManager.h"
#include "OriginalAreaAdapter.h"
#include "Optimized/RadiusExecuter.h"
#include "TaskExecuter.h"
#include "UpdateTickPlan.h"
#include "ObjectDispatchTrace.h"
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>

namespace RadiusRuntime
{
    enum class TraceStage
    {
        Wake,
        Queue,
        Begin,
        Members,
        End
    };

    struct TraceEvent
    {
        TraceStage stage = TraceStage::Wake;
        int64_t timeNs = 0, deadlineTick = 0, queuedNs = 0;
        int32_t originX = -1, originY = -1;
        int32_t worker = -1, queueWorker = -1;
        bool timer = false;
        RadiusQueueTrace queueTrace{};
    };

    inline std::string environment_value(const char* name)
    {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const auto value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }
} // namespace RadiusRuntime

template <class AreaType> class RuntimeBackend : public Ticker
{
    struct ObjectSchedule
    {
        Actor* actor = nullptr;
        Worker* worker = nullptr;
        std::shared_ptr<TaskExecuter> executer;
        int64_t nextTick = 0;
        uint64_t actorTag = 0;
        std::unique_ptr<ObjectDispatchSample> dispatchSample{};
    };

    class ObjectTimerTask final : public Task
    {
        RuntimeBackend* m_backend = nullptr;
        ObjectSchedule* m_schedule = nullptr;

    public:
        ObjectTimerTask(RuntimeBackend* backend, ObjectSchedule* schedule)
            : m_backend(backend)
            , m_schedule(schedule)
        {
        }
        void run() override
        {
            m_backend->dispatch_object(m_schedule);
        }
        uint64_t trace_tag() const override
        {
            return m_schedule->actorTag;
        }
    };

    class ObjectUpdateTask final : public Task
    {
        RuntimeBackend* m_backend = nullptr;
        ObjectSchedule* m_schedule = nullptr;

    public:
        ObjectUpdateTask(RuntimeBackend* backend, ObjectSchedule* schedule)
            : m_backend(backend)
            , m_schedule(schedule)
        {
        }
        void run() override
        {
            m_backend->update_object(m_schedule);
        }
    };

    class LegacyArea final : public Original::Area
    {
    public:
        RuntimeBackend* backend = nullptr;
        AreaType* payload = nullptr;
        LegacyArea(int32_t, int32_t)
        {
        }

    private:
        void do_update() override
        {
            backend->update_legacy(this);
        }
    };

    class LegacyTimerTask final : public Task
    {
        RuntimeBackend* m_backend = nullptr;
        LegacyArea* m_area = nullptr;

    public:
        LegacyTimerTask(RuntimeBackend* backend, LegacyArea* area)
            : m_backend(backend)
            , m_area(area)
        {
        }
        void run() override
        {
            m_backend->enqueue_legacy(m_area);
        }
    };

    class RadiusPayload final : public RadiusArea
    {
    public:
        RuntimeBackend* backend = nullptr;
        AreaType* payload = nullptr;
        int64_t nextTick = 0;
        size_t bucket = 0;
        RadiusPayload(int32_t, int32_t)
        {
        }

    private:
        void do_update() override
        {
            backend->update_radius(this);
        }
    };

    class RadiusTimerTask final : public Task
    {
        RuntimeBackend* m_backend = nullptr;
        RadiusPayload* m_area = nullptr;

    public:
        RadiusTimerTask(RuntimeBackend* backend, RadiusPayload* area)
            : m_backend(backend)
            , m_area(area)
        {
        }
        void run() override
        {
            if (m_backend->m_running.load())
            {
                m_backend->trace_radius_queue(m_area, true);
                m_backend->m_radiusExecuter->push(m_area, m_area->bucket);
            }
        }
    };

    template <size_t Capacity> class RadiusTimerBatchTask final : public Task
    {
        RuntimeBackend* m_backend = nullptr;
        std::array<RadiusPayload*, Capacity> m_areas{};
        size_t m_count = 0;

    public:
        RadiusTimerBatchTask(RuntimeBackend* backend, RadiusPayload* const* areas, size_t count)
            : m_backend(backend)
            , m_count(count)
        {
            std::copy_n(areas, count, m_areas.begin());
        }
        void run() override
        {
            if (m_backend->m_running.load() == false)
                return;

            if (m_backend->m_radiusPublishBatch)
            {
                std::array<RadiusSubmission, Capacity> submissions{};
                for (size_t index = 0; index < m_count; ++index)
                {
                    m_backend->trace_radius_queue(m_areas[index], true);
                    submissions[index] = {m_areas[index], m_areas[index]->bucket};
                }
                m_backend->m_radiusExecuter->push_batch(submissions.data(), m_count);
                return;
            }

            for (size_t index = 0; index < m_count; ++index)
            {
                m_backend->trace_radius_queue(m_areas[index], true);
                m_backend->m_radiusExecuter->push(m_areas[index], m_areas[index]->bucket);
            }
        }
    };

    struct RadiusPending
    {
        // Indexed by Worker id; only that Worker writes until it has joined.
        std::array<RadiusPayload*, 64> areas{};
        size_t count = 0;
        uint64_t reservations = 0;
        uint64_t tasks = 0;
    };

    std::string m_mode;
    AreaExecuter& m_areaExecuter;
    Worker* m_workers = nullptr;
    size_t m_workerCount = 0;
    size_t m_side = 0;
    int m_radius = 0;
    std::atomic_bool m_running{true};
    std::atomic_size_t m_legacyQueued{0};
    std::atomic_uint64_t m_objectSkippedTicks{0};
    std::unique_ptr<Original::AreaExecuter> m_legacy;
    AreaManager<LegacyArea> m_legacyAreas;
    std::unique_ptr<RadiusLocks> m_radiusLocks{};
    std::unique_ptr<RadiusExecuter> m_radiusExecuter{};
    AreaManager<RadiusPayload> m_radiusAreas;
    std::atomic_uint64_t m_radiusSkippedTicks{0};
    bool m_radiusTimerBatch = false;
    bool m_radiusPublishBatch = false;
    bool m_radiusSortTimers = true;
    std::vector<RadiusPending> m_radiusPending{};
    std::function<void(AreaType*, const RadiusRuntime::TraceEvent&)> m_radiusObserver{};
    size_t m_radiusTraceStride = 1;
    bool m_radiusTracePowerOfTwo = true;
    std::unique_ptr<TaskDispatcher> m_dispatcher;
    std::vector<std::unique_ptr<ObjectSchedule>> m_objects;
    std::vector<size_t> m_occupancy;
    uint64_t m_dispatchSampleStride = 0;
    ObjectDispatchProfile m_dispatchProfile{};
    uint64_t m_dispatcherProfileStride = 0;
    std::vector<DispatcherWorkerStatistics> m_dispatcherStatistics{};

    static bool& legacy_has_work()
    {
        static thread_local bool value = false;
        return value;
    }

    static AreaType*& radius_context()
    {
        static thread_local AreaType* area = nullptr;
        return area;
    }

    void trace_radius_queue(RadiusPayload* area, bool timer)
    {
        if (radius_trace_selected(area->payload) == false)
            return;

        RadiusRuntime::TraceEvent event{};
        event.stage = RadiusRuntime::TraceStage::Queue;
        event.timeNs = object_trace_now_ns();
        event.deadlineTick = area->nextTick;
        event.timer = timer;
        event.worker = currentWorker != nullptr ? static_cast<int32_t>(currentWorker->get_id()) : -1;
        m_radiusObserver(area->payload, event);
    }

    bool radius_trace_selected(const AreaType* area) const
    {
        if (m_radiusObserver == nullptr)
            return false;
        if (m_radiusTraceStride == 1)
            return true;
        const auto tag =
            static_cast<uint64_t>(area->get_x()) * 73856093 ^ static_cast<uint64_t>(area->get_y()) * 19349663;
        if (m_radiusTracePowerOfTwo)
            return (tag & (m_radiusTraceStride - 1)) == 0;
        return tag % m_radiusTraceStride == 0;
    }

    void update_radius_observed(RadiusPayload* area)
    {
        auto& context = radius_context();
        const auto previous = context;
        context = area->payload;
        RadiusRuntime::TraceEvent event{};
        event.stage = RadiusRuntime::TraceStage::Begin;
        event.timeNs = object_trace_now_ns();
        event.deadlineTick = area->nextTick;
        event.worker = currentWorker != nullptr ? static_cast<int32_t>(currentWorker->get_id()) : -1;
        try
        {
            const auto trace = m_radiusExecuter->queue_trace(area);
            if (trace != nullptr)
                event.queueTrace = *trace;
            m_radiusObserver(area->payload, event);
            area->payload->update();
            event.stage = RadiusRuntime::TraceStage::End;
            event.timeNs = object_trace_now_ns();
            m_radiusObserver(area->payload, event);
        }
        catch (...)
        {
            context = previous;
            throw;
        }
        context = previous;
    }

    void enqueue_legacy(LegacyArea* area)
    {
        if (m_running.load() == false)
            return;
        ++m_legacyQueued;
        m_legacy->push(area);
    }

    void update_legacy(LegacyArea* area)
    {
        --m_legacyQueued;
        area->payload->update();
        if (m_running.load())
            currentWorker->add_task(new LegacyTimerTask(this, area), 50);
    }

    void schedule_radius(AreaType* payload)
    {
        if (m_running.load() == false)
            return;
        if (radius_trace_selected(payload))
        {
            RadiusRuntime::TraceEvent event{};
            event.timeNs = object_trace_now_ns();
            auto origin = radius_context();
            if (origin != nullptr)
            {
                event.originX = origin->get_x();
                event.originY = origin->get_y();
            }
            m_radiusObserver(payload, event);
        }
        if (payload->request_schedule() == false)
            return;

        auto area = m_radiusAreas.get_area(payload->get_x(), payload->get_y());
        area->nextTick = get_now_tick();
        trace_radius_queue(area, false);
        m_radiusExecuter->push(area, area->bucket);
    }

    void update_radius(RadiusPayload* area)
    {
        if (radius_trace_selected(area->payload))
        {
            update_radius_observed(area);
        }
        else
        {
            area->payload->update();
        }
        if (area->payload->finish_schedule() == false || m_running.load() == false)
            return;

        const auto plan = next_update_tick(area->nextTick, get_now_tick());
        area->nextTick = (std::max)(get_now_tick(), area->payload->next_actor_tick(plan.nextTick));
        if (plan.skippedTicks != 0)
            m_radiusSkippedTicks.fetch_add(plan.skippedTicks, std::memory_order_relaxed);
        auto& pending = m_radiusPending[currentWorker->get_id()];
        ++pending.reservations;
        if (m_radiusTimerBatch)
        {
            pending.areas[pending.count++] = area;
        }
        else
        {
            ++pending.tasks;
            currentWorker->add_task_at(new RadiusTimerTask(this, area), area->nextTick);
        }
    }

    Task* radius_timer_task(RadiusPayload* const* areas, size_t count)
    {
        if (count == 1)
            return new RadiusTimerTask(this, areas[0]);
        if (count <= 2)
            return new RadiusTimerBatchTask<2>(this, areas, count);
        if (count <= 4)
            return new RadiusTimerBatchTask<4>(this, areas, count);
        if (count <= 8)
            return new RadiusTimerBatchTask<8>(this, areas, count);
        if (count <= 16)
            return new RadiusTimerBatchTask<16>(this, areas, count);
        if (count <= 32)
            return new RadiusTimerBatchTask<32>(this, areas, count);

        return new RadiusTimerBatchTask<64>(this, areas, count);
    }

    void flush_radius_timers()
    {
        auto& pending = m_radiusPending[currentWorker->get_id()];
        std::array<RadiusPayload*, 64> ordered{};
        auto areas = pending.areas.data();
        if (m_radiusSortTimers && pending.count > 1)
        {
            std::array<size_t, 64> indices{};
            for (size_t index = 0; index < pending.count; ++index)
                indices[index] = index;
            std::sort(indices.begin(), indices.begin() + pending.count,
                      [&](size_t left, size_t right)
                      {
                          const auto leftTick = pending.areas[left]->nextTick;
                          const auto rightTick = pending.areas[right]->nextTick;
                          return leftTick != rightTick ? leftTick < rightTick : left < right;
                      });
            for (size_t index = 0; index < pending.count; ++index)
                ordered[index] = pending.areas[indices[index]];
            areas = ordered.data();
        }

        size_t begin = 0;
        while (begin < pending.count)
        {
            // Preserve exact deadlines and the original relative order of equal deadlines.
            const auto deadline = areas[begin]->nextTick;
            size_t end = begin + 1;
            while (end < pending.count && areas[end]->nextTick == deadline)
                ++end;

            auto task = radius_timer_task(areas + begin, end - begin);
            currentWorker->add_task_at(task, deadline);
            ++pending.tasks;
            begin = end;
        }
        pending.count = 0;
    }

    void dispatch_object(ObjectSchedule* schedule)
    {
        if (m_running.load() == false)
            return;
        if (schedule->dispatchSample != nullptr)
        {
            schedule->dispatchSample->submittedNs = object_trace_now_ns();
            schedule->dispatchSample->deadlineTick = schedule->nextTick;
        }
        schedule->executer->run_task(new ObjectUpdateTask(this, schedule));
    }

    void update_object(ObjectSchedule* schedule)
    {
        if (m_running.load() == false)
            return;
        auto sample = schedule->dispatchSample.get();
        const auto submittedNs = sample != nullptr ? sample->submittedNs : int64_t{0};
        const auto timerLateMs = sample != nullptr
                                     ? (std::max)(int64_t{0}, sample->submittedNs / 1000000 - sample->deadlineTick)
                                     : int64_t{0};
        const auto beginNs = sample != nullptr ? object_trace_now_ns() : int64_t{0};
        schedule->actor->update();
        if (sample != nullptr)
        {
            const auto actorNs = object_trace_now_ns() - beginNs;
            if (sample->initialUpdate)
            {
                sample->initialUpdate = false;
            }
            else
            {
                record_object_dispatch(sample->statistics, static_cast<uint64_t>(beginNs - submittedNs),
                                       static_cast<uint64_t>(actorNs), static_cast<uint64_t>(timerLateMs));
            }
        }

        if (m_running.load())
        {
            const auto plan = next_update_tick(schedule->nextTick, get_now_tick());
            schedule->nextTick = plan.nextTick;
            if (plan.skippedTicks != 0)
                m_objectSkippedTicks.fetch_add(plan.skippedTicks, std::memory_order_relaxed);

            schedule->worker->add_task_at(new ObjectTimerTask(this, schedule), plan.nextTick);
        }
    }

    size_t cell_index(const AreaType* area) const
    {
        return static_cast<size_t>(area->get_y()) * m_side + area->get_x();
    }

public:
    // Diagnostic only; configure after prepare and before starting Workers.
    void configure_radius_trace(std::function<void(AreaType*, const RadiusRuntime::TraceEvent&)> observer,
                                size_t stride = 1)
    {
        if (stride == 0)
            throw std::invalid_argument("Radius trace stride must be positive");
        m_radiusObserver = std::move(observer);
        m_radiusTraceStride = stride;
        m_radiusTracePowerOfTwo = (stride & (stride - 1)) == 0;
        m_radiusAreas.work(
            [&](int, int, RadiusPayload* area)
            {
                m_radiusExecuter->configure_queue_trace(area, radius_trace_selected(area->payload));
            });
    }

    RuntimeBackend(std::string mode, AreaExecuter& areaExecuter, Worker* workerPool, size_t workerCount, size_t radius,
                   uint64_t dispatchSampleStride = 0, uint64_t dispatcherProfileStride = 0)
        : m_mode(std::move(mode))
        , m_areaExecuter(areaExecuter)
        , m_workers(workerPool)
        , m_workerCount(workerCount)
        , m_radius(static_cast<int>(radius))
        , m_dispatchSampleStride(dispatchSampleStride)
        , m_dispatcherProfileStride(dispatcherProfileStride)
    {
        m_dispatchProfile.workerSamples.resize(workerCount, 0);
        if (m_mode == "original")
            m_legacy = std::make_unique<Original::AreaExecuter>(workerCount);
        if (m_mode == "legacy")
        {
            if (workerCount == 0)
                throw std::invalid_argument("Radius scheduling requires Workers");
            m_radiusExecuter = std::make_unique<RadiusExecuter>(workerCount);
            const auto queueDiagnostics = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS");
            if (queueDiagnostics.empty() == false && queueDiagnostics != "0" && queueDiagnostics != "1")
                throw std::invalid_argument("Radius queue diagnostics must be 0 or 1");
            m_radiusExecuter->configure_queue_diagnostics(queueDiagnostics == "1");
            m_radiusPending.resize(workerCount);
            const auto batchText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_BATCH");
            const auto profileText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_PROFILE");
            const auto timerText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_TIMER_BATCH");
            const auto publishText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_PUBLISH_BATCH");
            const auto sortText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_SORT_TIMERS");
            if (sortText.empty() == false && sortText != "0" && sortText != "1")
                throw std::invalid_argument("Radius timer ordering must be 0 or 1");
            m_radiusSortTimers = sortText.empty() || sortText == "1";
            const size_t batch = batchText.empty() == false ? std::stoull(batchText) : size_t{64};
            const size_t stride = profileText.empty() == false ? std::stoull(profileText) : size_t{0};
            m_radiusTimerBatch = timerText.empty() || timerText == "1";
            m_radiusPublishBatch = publishText.empty() || publishText == "1";
            const auto fastText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_FAST_BUCKET");
            const auto mergeText = RadiusRuntime::environment_value("AREAUPDATE_RADIUS_MERGE_NOTIFY");
            m_radiusExecuter->configure_publication(fastText.empty() || fastText == "1",
                                                    mergeText.empty() || mergeText == "1");
            m_radiusExecuter->configure(batch, stride,
                                        [this]
                                        {
                                            flush_radius_timers();
                                        });
        }
        if (m_mode == "job")
            m_dispatcher = std::make_unique<TaskDispatcher>(workerCount, m_dispatcherProfileStride);
    }
    ~RuntimeBackend()
    {
        stop();
        finish();
    }

    void print_radius_profile() const
    {
        if (m_radiusExecuter == nullptr)
            return;

        m_radiusExecuter->print_profile();
        uint64_t reservations = 0, tasks = 0;
        for (const auto& pending : m_radiusPending)
        {
            reservations += pending.reservations;
            tasks += pending.tasks;
        }
        std::printf("RADIUS_TIMERS grouped=%d publish_batch=%d sort=%d reservations=%llu tasks=%llu\n",
                    static_cast<int>(m_radiusTimerBatch), static_cast<int>(m_radiusPublishBatch),
                    static_cast<int>(m_radiusSortTimers), reservations, tasks);
    }

    Ticker* ticker()
    {
        if (m_mode == "area")
            return &m_areaExecuter;
        if (m_mode == "legacy")
            return m_radiusExecuter.get();
        return m_mode == "original" ? this : nullptr;
    }

    void prepare(AreaManager<AreaType>& areas)
    {
        m_side = static_cast<size_t>(areas.get_count_x());
        if (m_mode == "job")
            m_occupancy.resize(m_side * m_side, 0);
        if (m_mode == "legacy")
        {
            const auto rows = areas.get_count_y();
            m_radiusLocks = std::make_unique<RadiusLocks>(m_side * static_cast<size_t>(rows));
            m_radiusAreas.build(0, 0, static_cast<float>(m_side), static_cast<float>(rows), 1, 1);
            m_radiusAreas.work(
                [&](int x, int y, RadiusPayload* area)
                {
                    area->backend = this;
                    area->payload = areas.get_area(x, y);
                    area->bucket = (static_cast<size_t>(y) * m_side + x) % m_workerCount;
                    std::vector<size_t> cells{};
                    cells.reserve(static_cast<size_t>((2 * m_radius + 1) * (2 * m_radius + 1)));
                    for (int ny = (std::max)(0, y - m_radius); ny <= (std::min)(rows - 1, y + m_radius); ++ny)
                    {
                        for (int nx = (std::max)(0, x - m_radius);
                             nx <= (std::min)(static_cast<int>(m_side) - 1, x + m_radius); ++nx)
                        {
                            cells.push_back(static_cast<size_t>(ny) * m_side + nx);
                        }
                    }
                    area->set_footprint(m_radiusLocks->footprint(std::move(cells)));
                    area->payload->set_wake(
                        [this](Area* payload)
                        {
                            schedule_radius(static_cast<AreaType*>(payload));
                        });
                });
            return;
        }
        if (m_mode != "original")
            return;

        m_legacyAreas.build(0, 0, static_cast<float>(m_side), static_cast<float>(m_side), 1, 1);
        m_legacyAreas.work(
            [&](int x, int y, LegacyArea* area)
            {
                area->backend = this;
                area->payload = areas.get_area(x, y);
                for (int ny = (std::max)(0, y - m_radius); ny <= (std::min)(static_cast<int>(m_side) - 1, y + m_radius);
                     ++ny)
                {
                    for (int nx = (std::max)(0, x - m_radius);
                         nx <= (std::min)(static_cast<int>(m_side) - 1, x + m_radius); ++nx)
                    {
                        if (nx != x || ny != y)
                            area->add_ref(m_legacyAreas.get_area(nx, ny));
                    }
                }
            });
        m_legacyAreas.work(
            [this](int, int, LegacyArea* area)
            {
                enqueue_legacy(area);
            });
    }

    void add_object(Actor* actor, AreaType* area)
    {
        if (m_mode != "job")
        {
            area->add_object(actor);
            return;
        }

        if (m_occupancy[cell_index(area)]++ == 0)
            area->set_active(true);
        auto schedule = std::make_unique<ObjectSchedule>();
        schedule->actor = actor;
        schedule->worker = &m_workers[m_objects.size() % m_workerCount];
        schedule->executer = std::make_shared<TaskExecuter>(*m_dispatcher);
        schedule->nextTick = get_now_tick();
        schedule->actorTag = m_objects.size() + 1;
        if (sample_object_dispatch(schedule->actorTag, m_dispatchSampleStride))
            schedule->dispatchSample = std::make_unique<ObjectDispatchSample>();
        auto pointer = schedule.get();
        m_objects.emplace_back(std::move(schedule));
        pointer->worker->add_task(new ObjectTimerTask(this, pointer));
    }

    bool move_object(Actor* actor, AreaType* source, AreaType* destination)
    {
        if (m_mode != "job")
            return source->move_object(actor, destination);
        if (--m_occupancy[cell_index(source)] == 0)
            source->set_active(false);
        if (m_occupancy[cell_index(destination)]++ == 0)
            destination->set_active(true);
        return true;
    }

    void stop()
    {
        m_running = false;
    }

    uint64_t legacy_updates() const
    {
        return Original::RUN_COUNT.load();
    }
    uint64_t legacy_failures() const
    {
        return Original::FAIL_COUNT.load();
    }
    uint64_t radius_updates() const
    {
        return m_radiusExecuter != nullptr ? m_radiusExecuter->updates() : 0;
    }
    uint64_t radius_failures() const
    {
        return m_radiusExecuter != nullptr ? m_radiusExecuter->failures() : 0;
    }
    uint64_t radius_skipped_ticks() const
    {
        return m_radiusSkippedTicks.load();
    }
    uint64_t object_skipped_ticks() const
    {
        return m_objectSkippedTicks.load();
    }
    const ObjectDispatchProfile& dispatch_profile() const
    {
        if (m_dispatcher != nullptr)
            throw std::logic_error("Read dispatch profile only after finish");
        return m_dispatchProfile;
    }
    const std::vector<DispatcherWorkerStatistics>& dispatcher_statistics() const
    {
        if (m_dispatcher != nullptr)
            throw std::logic_error("Read Dispatcher statistics only after finish");
        return m_dispatcherStatistics;
    }

    void finish()
    {
        if (m_dispatcher != nullptr)
        {
            m_dispatcher->shutdown();
            // Core Workers must already be joined, as required by the shutdown contract.
            for (const auto& schedule : m_objects)
            {
                if (schedule->dispatchSample == nullptr)
                    continue;
                ++m_dispatchProfile.selectedActors;
                const auto& statistics = schedule->dispatchSample->statistics;
                merge_object_dispatch(m_dispatchProfile.statistics, statistics);
                m_dispatchProfile.workerSamples[schedule->worker->get_id()] += statistics.samples;
            }
            m_dispatcherStatistics = m_dispatcher->worker_statistics();
            m_objects.clear();
            m_dispatcher.reset();
        }
    }

    bool on_tick() override
    {
        const bool empty = m_legacy->run();
        legacy_has_work() = empty == false;
        return empty;
    }
    bool has_work() const override
    {
        return legacy_has_work();
    }
    TickerIdleState idle_state() const override
    {
        return {m_legacyQueued.load(), 0, legacy_has_work()};
    }
};
