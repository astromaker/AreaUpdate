#include "MotionInteractionProbe.h"
#include "AreaManager.h"
#include "ComparisonTiming.h"
#include "../../Examples/AreaUpdateExample/RuntimeBackend.h"
#include "../../Examples/AreaUpdateExample/TimerTraceOutput.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <random>
#include <array>
#include <functional>

namespace
{
    int64_t motion_now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    class ProbeArea;
    class ProbeWorld;
    thread_local ProbeArea* currentProbeArea = nullptr;

    uint64_t mix_value(uint64_t value)
    {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        return value;
    }

    struct ProbeEvent
    {
        uint64_t id = 0, damage = 0;
    };

    struct ProbeAreaTrace
    {
        std::mutex mutex;
        std::array<RadiusRuntime::TraceEvent, 64> events{};
        size_t next = 0, count = 0;
        RadiusRuntime::TraceEvent queued{}, begin{};
    };

    class ProbeArea : public Area
    {
        std::unique_ptr<ProbeAreaTrace> m_trace{};

    public:
        ProbeArea(int x, int y)
            : Area(x, y)
        {
        }
        void enable_trace()
        {
            m_trace = std::make_unique<ProbeAreaTrace>();
        }
        void record_trace(RadiusRuntime::TraceEvent event)
        {
            if (m_trace == nullptr)
                return;

            std::lock_guard<std::mutex> guard(m_trace->mutex);
            if (event.stage == RadiusRuntime::TraceStage::Queue)
                m_trace->queued = event;
            if (event.stage == RadiusRuntime::TraceStage::Begin)
            {
                event.queuedNs = m_trace->queued.timeNs;
                event.timer = m_trace->queued.timer;
                event.queueWorker = m_trace->queued.worker;
                m_trace->begin = event;
            }
            if (event.stage == RadiusRuntime::TraceStage::Members)
            {
                event.queuedNs = m_trace->begin.queuedNs;
                event.deadlineTick = m_trace->begin.deadlineTick;
                event.timer = m_trace->begin.timer;
                event.queueWorker = m_trace->begin.queueWorker;
            }
            m_trace->events[m_trace->next] = event;
            m_trace->next = (m_trace->next + 1) % m_trace->events.size();
            m_trace->count = (std::min)(m_trace->count + 1, m_trace->events.size());
        }
        std::vector<RadiusRuntime::TraceEvent> trace_events(int64_t start, int64_t end)
        {
            std::vector<RadiusRuntime::TraceEvent> events{};
            events.reserve(m_trace->events.size());
            std::lock_guard<std::mutex> guard(m_trace->mutex);
            const auto first = (m_trace->next + m_trace->events.size() - m_trace->count) % m_trace->events.size();
            for (size_t index = 0; index < m_trace->count; ++index)
            {
                const auto& event = m_trace->events[(first + index) % m_trace->events.size()];
                if (event.timeNs >= start - 100000000 && event.timeNs <= end)
                    events.push_back(event);
            }
            return events;
        }

    private:
        void do_update() override
        {
            currentProbeArea = this;
            if (m_trace != nullptr)
            {
                RadiusRuntime::TraceEvent event{};
                event.stage = RadiusRuntime::TraceStage::Members;
                event.timeNs = motion_now_ns();
                record_trace(event);
            }
        }
    };

    struct ProbeMoveTrace
    {
        int64_t requestedNs = 0, acceptedNs = 0;
        ProbeArea* source = nullptr;
        ProbeArea* destination = nullptr;
    };

    struct ProbeActorTrace
    {
        std::mutex mutex;
        int64_t previousNs = 0;
        ProbeArea* previousArea = nullptr;
        std::vector<ProbeMoveTrace> moves{};
        size_t moveCount = 0;
        ProbeActorTrace()
        {
            moves.reserve(8);
        }
    };

    struct ProbeAreaHistory
    {
        ProbeArea* area = nullptr;
        std::vector<RadiusRuntime::TraceEvent> events{};
    };

    struct ProbeLateTrace
    {
        size_t actorId = 0, moveCount = 0;
        int64_t startNs = 0, endNs = 0, observerWaitNs = 0;
        int64_t actorDeadline = 0;
        ProbeArea* source = nullptr;
        ProbeArea* destination = nullptr;
        std::vector<ProbeMoveTrace> moves{};
        std::vector<ProbeAreaHistory> areas{};
    };

    void observe_trace_max(std::atomic_int64_t& target, int64_t value)
    {
        auto current = target.load(std::memory_order_relaxed);
        while (current < value && target.compare_exchange_weak(current, value, std::memory_order_relaxed) == false)
        {
        }
    }

    class ProbeActor : public Actor
    {
        friend class ProbeWorld;
        ProbeWorld& m_world;
        size_t m_id = 0;
        uint64_t m_random = 1, m_health = 1000, m_damage = 0;
        int64_t m_nextAttack = 0;
        std::mutex m_inboxLock;
        std::vector<ProbeEvent> m_inbox{};
        std::vector<uint64_t> m_appliedIds{};
        std::atomic_bool m_updating{false};
        std::atomic_uint64_t m_updates{0};
        std::atomic<ProbeArea*> m_expected{nullptr}, m_observed{nullptr};
        uint64_t m_moveMarker = 0;
        int64_t m_lastUpdateTick = 0;
        uint64_t m_maxGapMs = 0;
        std::array<uint64_t, 128> m_gapHistogram{};
        std::unique_ptr<ProbeActorTrace> m_trace{};

    public:
        ProbeActor(ProbeWorld& world, size_t id)
            : m_world(world)
            , m_id(id)
            , m_random(id + 1)
        {
        }
        void post(ProbeEvent event)
        {
            std::lock_guard<std::mutex> guard(m_inboxLock);
            m_inbox.push_back(event);
        }
        void update() override;
    };

    class ProbeWorld
    {
        friend class ProbeActor;
        static constexpr size_t WORKERS = 8;
        AreaExecuter m_executor{2};
        std::unique_ptr<Worker[]> m_workers = std::make_unique<Worker[]>(WORKERS);
        AreaManager<ProbeArea> m_areas;
        std::unique_ptr<RuntimeBackend<ProbeArea>> m_backend{};
        std::vector<std::unique_ptr<ProbeActor>> m_actors{};
        std::vector<std::vector<uint64_t>> m_expectedIds{};
        std::atomic_uint64_t m_sent{0}, m_applied{0}, m_violations{0};
        std::atomic_bool m_combat{false};
        std::atomic_bool m_measureGaps{false};
        bool m_areaMode = true, m_verify = true, m_corruptEvents = false, m_boundary = false;
        int m_side = 64;
        std::mt19937 m_random;
        uint64_t m_moves = 0;
        size_t m_moveAttempts = 64;
        bool m_settledMoves = false;
        int64_t m_traceThresholdNs = 0;
        uint64_t m_timerTraceMs = 0;
        std::atomic_int64_t m_captureMinimumNs{0};
        std::atomic_uint64_t m_lateZero{0}, m_lateOne{0}, m_lateMany{0};
        std::atomic_int64_t m_maxZeroNs{0}, m_maxOneNs{0}, m_maxManyNs{0};
        std::mutex m_lateLock;
        std::vector<ProbeLateTrace> m_late{};

        bool move_actor(ProbeActor& actor, ProbeArea* source, ProbeArea* destination)
        {
            if (actor.m_trace == nullptr)
                return m_backend->move_object(&actor, source, destination);

            std::lock_guard<std::mutex> guard(actor.m_trace->mutex);
            const auto requested = motion_now_ns();
            if (m_backend->move_object(&actor, source, destination) == false)
                return false;

            if (actor.m_trace->moves.size() == 8)
                actor.m_trace->moves.erase(actor.m_trace->moves.begin());
            actor.m_trace->moves.push_back({requested, motion_now_ns(), source, destination});
            ++actor.m_trace->moveCount;
            return true;
        }

        void trace_actor_tick(ProbeActor& actor, ProbeArea* area)
        {
            if (actor.m_trace == nullptr)
                return;

            const auto now = motion_now_ns();
            ProbeLateTrace late{};
            {
                std::lock_guard<std::mutex> guard(actor.m_trace->mutex);
                auto& state = *actor.m_trace;
                const auto gap = state.previousNs == 0 ? int64_t{0} : now - state.previousNs;
                if (m_measureGaps.load() && gap >= m_traceThresholdNs)
                {
                    if (state.moveCount == 0)
                    {
                        ++m_lateZero;
                        observe_trace_max(m_maxZeroNs, gap);
                    }
                    else if (state.moveCount == 1)
                    {
                        ++m_lateOne;
                        observe_trace_max(m_maxOneNs, gap);
                    }
                    else
                    {
                        ++m_lateMany;
                        observe_trace_max(m_maxManyNs, gap);
                    }
                    if (gap >= m_captureMinimumNs.load())
                    {
                        late.actorId = actor.m_id;
                        late.startNs = state.previousNs;
                        late.endNs = now;
                        late.actorDeadline = actor.get_update_deadline();
                        late.observerWaitNs = motion_now_ns() - now;
                        late.source = state.previousArea;
                        late.destination = area;
                        late.moves = state.moves;
                        late.moveCount = state.moveCount;
                    }
                }
                state.previousNs = m_measureGaps.load() ? now : 0;
                state.previousArea = area;
                state.moves.clear();
                state.moveCount = 0;
            }
            if (late.startNs == 0)
                return;

            std::vector<ProbeArea*> areas{late.source, late.destination};
            for (const auto& move : late.moves)
            {
                areas.push_back(move.source);
                areas.push_back(move.destination);
            }
            std::sort(areas.begin(), areas.end(), std::less<ProbeArea*>{});
            areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
            for (auto value : areas)
                late.areas.push_back({value, value->trace_events(late.startNs, late.endNs)});

            std::lock_guard<std::mutex> guard(m_lateLock);
            m_late.push_back(std::move(late));
            std::sort(m_late.begin(), m_late.end(),
                      [](const ProbeLateTrace& left, const ProbeLateTrace& right)
                      {
                          return left.endNs - left.startNs > right.endNs - right.startNs;
                      });
            if (m_late.size() > 16)
                m_late.pop_back();
            if (m_late.size() == 16)
                m_captureMinimumNs.store(m_late.back().endNs - m_late.back().startNs);
        }

        void print_late_traces()
        {
            if (m_traceThresholdNs == 0)
                return;

            std::printf("TRACE_SUMMARY threshold_ms=%.3f zero_moves=%llu one_move=%llu multi_moves=%llu "
                        "max_zero_ms=%.3f max_one_ms=%.3f max_multi_ms=%.3f retained=%zu\n",
                        m_traceThresholdNs / 1000000.0, m_lateZero.load(), m_lateOne.load(), m_lateMany.load(),
                        m_maxZeroNs.load() / 1000000.0, m_maxOneNs.load() / 1000000.0, m_maxManyNs.load() / 1000000.0,
                        m_late.size());
            for (size_t index = 0; index < m_late.size(); ++index)
            {
                const auto& late = m_late[index];
                std::printf("LATE_ACTOR sample=%zu actor=%zu start_ns=%lld end_ns=%lld gap_ms=%.3f moves=%zu "
                            "from_x=%d from_y=%d to_x=%d to_y=%d observer_wait_ms=%.3f actor_deadline_ms=%lld\n",
                            index, late.actorId, late.startNs, late.endNs, (late.endNs - late.startNs) / 1000000.0,
                            late.moveCount, late.source->get_x(), late.source->get_y(), late.destination->get_x(),
                            late.destination->get_y(), late.observerWaitNs / 1000000.0, late.actorDeadline);
                for (const auto& move : late.moves)
                {
                    std::printf("LATE_MOVE sample=%zu request_ns=%lld accepted_ns=%lld from_x=%d from_y=%d "
                                "to_x=%d to_y=%d\n",
                                index, move.requestedNs, move.acceptedNs, move.source->get_x(), move.source->get_y(),
                                move.destination->get_x(), move.destination->get_y());
                }
                for (const auto& history : late.areas)
                {
                    for (const auto& event : history.events)
                    {
                        std::printf("LATE_AREA sample=%zu x=%d y=%d stage=%d time_ns=%lld deadline_ms=%lld "
                                    "queued_ns=%lld timer=%d origin_x=%d origin_y=%d worker=%d queue_worker=%d "
                                    "published_ns=%lld first_probe_ns=%lld acquired_ns=%lld attempt_ns=%llu "
                                    "radius_failures=%llu\n",
                                    index, history.area->get_x(), history.area->get_y(), static_cast<int>(event.stage),
                                    event.timeNs, event.deadlineTick, event.queuedNs, static_cast<int>(event.timer),
                                    event.originX, event.originY, event.worker, event.queueWorker,
                                    event.queueTrace.publishedNs, event.queueTrace.firstProbeNs,
                                    event.queueTrace.acquiredNs, event.queueTrace.attemptNs, event.queueTrace.failures);
                    }
                }
            }
        }

        template <class Predicate> void wait_for(Predicate predicate, const char* reason)
        {
            const auto deadline = get_now_tick() + 10000;
            while (get_now_tick() < deadline)
            {
                if (predicate())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error(reason);
        }
        bool all_inactive()
        {
            bool inactive = true;
            m_areas.work(
                [&](int, int, ProbeArea* area)
                {
                    if (area->is_active())
                        inactive = false;
                });
            return inactive;
        }
        void settle_moves()
        {
            wait_for(
                [&]
                {
                    for (const auto& actor : m_actors)
                    {
                        if (actor->m_updates.load() < actor->m_moveMarker + 2
                            || actor->m_observed.load() != actor->m_expected.load())
                            return false;
                    }
                    return true;
                },
                "Actor did not settle in expected Area");
        }
        void detach_all()
        {
            for (const auto& actor : m_actors)
            {
                auto owner = actor->m_expected.load();
                if (owner == nullptr || owner->move_object(actor.get(), nullptr) == false)
                    throw std::runtime_error("Actor detach rejected after settlement");
            }
            wait_for(
                [&]
                {
                    return all_inactive();
                },
                "Areas stayed active after detach");
        }
        void ownership_cycle()
        {
            detach_all();
            std::vector<uint64_t> before{};
            for (const auto& actor : m_actors)
                before.push_back(actor->m_updates.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            for (size_t id = 0; id < m_actors.size(); ++id)
            {
                auto& actor = *m_actors[id];
                if (before[id] != actor.m_updates.load())
                    throw std::runtime_error("Detached Actor updated");
                post(id);
                auto area = m_areas.get_area(static_cast<int>(id % m_side), static_cast<int>(id / m_side % m_side));
                actor.m_expected.store(area);
                area->add_object(&actor);
                actor.m_moveMarker = actor.m_updates.load();
            }
            settle_moves();
            wait_for(
                [&]
                {
                    return m_sent.load() == m_applied.load();
                },
                "Rejoin lost queued events");
            detach_all();
        }
        void stop()
        {
            if (m_backend != nullptr)
                m_backend->stop();
            for (size_t id = 0; id < WORKERS; ++id)
                m_workers[id].stop();
            for (size_t id = 0; id < WORKERS; ++id)
                m_workers[id].join();
            if (m_backend != nullptr)
                m_backend->finish();
        }

    public:
        ProbeWorld(std::string mode, size_t actors, uint32_t seed, bool verify, bool hotspot, bool corruptEvents,
                   bool boundary, size_t moveAttempts, bool settledMoves, int64_t traceMs, uint64_t timerTraceMs)
            : m_areaMode(mode != "job")
            , m_verify(verify)
            , m_corruptEvents(corruptEvents)
            , m_boundary(boundary)
            , m_random(seed)
            , m_moveAttempts(moveAttempts)
            , m_settledMoves(settledMoves)
            , m_traceThresholdNs(traceMs * 1000000)
            , m_timerTraceMs(timerTraceMs)
        {
            m_captureMinimumNs.store(m_traceThresholdNs);
            m_late.reserve(traceMs != 0 ? 16 : 0);
            m_areas.build(0, 0, static_cast<float>(m_side), static_cast<float>(m_side), 1, 1);
            if (mode == "area")
            {
                m_areas.work(
                    [&](int, int, ProbeArea* area)
                    {
                        area->set_wake(
                            [&](Area* value)
                            {
                                m_executor.push(value);
                            });
                    });
            }
            m_backend = std::make_unique<RuntimeBackend<ProbeArea>>(mode, m_executor, m_workers.get(), WORKERS, 2);
            m_backend->prepare(m_areas);
            if (traceMs != 0)
            {
                m_areas.work(
                    [](int, int, ProbeArea* area)
                    {
                        area->enable_trace();
                    });
                m_backend->configure_radius_trace(
                    [](ProbeArea* area, const RadiusRuntime::TraceEvent& event)
                    {
                        area->record_trace(event);
                    });
            }

            m_expectedIds.resize(verify ? actors : 0);
            for (size_t id = 0; id < actors; ++id)
            {
                m_actors.push_back(std::make_unique<ProbeActor>(*this, id));
                if (traceMs != 0)
                    m_actors.back()->m_trace = std::make_unique<ProbeActorTrace>();
            }

            for (const auto& actor : m_actors)
            {
                const auto x = m_boundary ? m_side / 2 - 1 + static_cast<int>(m_random() % 2)
                                          : (hotspot ? m_side / 2 : static_cast<int>(m_random() % m_side));
                const auto y = m_boundary ? m_side / 2 - 1 + static_cast<int>(m_random() % 2)
                                          : (hotspot ? m_side / 2 : static_cast<int>(m_random() % m_side));
                auto area = m_areas.get_area(x, y);
                actor->m_expected.store(area);
                m_backend->add_object(actor.get(), area);
            }
        }
        ~ProbeWorld()
        {
            stop();
        }
        void post(size_t target)
        {
            const auto id = m_sent.fetch_add(1) + 1;
            if (m_verify)
                m_expectedIds[target].push_back(id);
            ProbeEvent event{id, 1 + mix_value(id) % 31};
            if (m_verify && m_corruptEvents && id == 17)
                event.id = 16;

            m_actors[target]->post(event);
        }
        void run(int seconds, const char* mode, const char* label, uint32_t seed, const char* layout)
        {
            for (size_t id = 0; id < WORKERS; ++id)
            {
                if (m_timerTraceMs != 0)
                    m_workers[id].set_timer_trace_threshold(m_timerTraceMs);
                m_workers[id].start(m_backend->ticker(), id);
            }

            m_combat.store(m_verify == false);
            const auto begin = get_now_tick();
            const auto cpuBegin = Comparison::process_cpu_seconds();
            uint64_t baseline = 0;
            int64_t steadyBegin = 0;
            while (get_now_tick() - begin < seconds * 1000)
            {
                for (size_t attempt = 0; attempt < m_moveAttempts; ++attempt)
                {
                    auto& actor = *m_actors[m_random() % m_actors.size()];
                    if (m_settledMoves && actor.m_updates.load() <= actor.m_moveMarker)
                        continue;
                    auto source = actor.m_expected.load();
                    const auto low = m_boundary ? m_side / 2 - 1 : 0;
                    const auto high = m_boundary ? m_side / 2 : m_side - 1;
                    const auto x =
                        (std::max)(low, (std::min)(high, source->get_x() + static_cast<int>(m_random() % 3) - 1));
                    const auto y =
                        (std::max)(low, (std::min)(high, source->get_y() + static_cast<int>(m_random() % 3) - 1));
                    auto destination = m_areas.get_area(x, y);
                    if (destination != source && move_actor(actor, source, destination))
                    {
                        actor.m_expected.store(destination);
                        actor.m_moveMarker = actor.m_updates.load();
                        ++m_moves;
                    }
                }
                if (m_verify)
                {
                    for (int event = 0; event < 128; ++event)
                        post(m_random() % m_actors.size());
                }
                if (steadyBegin == 0 && get_now_tick() - begin >= 1000)
                {
                    for (const auto& actor : m_actors)
                        baseline += actor->m_updates.load();
                    steadyBegin = get_now_tick();
                    m_measureGaps.store(true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto end = get_now_tick();
            m_measureGaps.store(false);
            const auto cpu = Comparison::process_cpu_seconds() - cpuBegin;
            uint64_t updates = 0;
            for (const auto& actor : m_actors)
                updates += actor->m_updates.load();

            m_combat.store(false);
            for (const auto& actor : m_actors)
            {
                wait_for(
                    [&]
                    {
                        return actor->m_updating.load() == false;
                    },
                    "Combat did not finish");
            }
            settle_moves();
            wait_for(
                [&]
                {
                    return m_sent.load() == m_applied.load();
                },
                "Events did not drain");
            if (m_verify && m_areaMode)
                ownership_cycle();
            stop();

            if (m_areaMode == false && m_executor.idle_state().readyWork != 0)
                throw std::runtime_error("Object probe unexpectedly queued Area work");

            uint64_t damage = 0, healthChecksum = 0, maxGap = 0, gapSamples = 0;
            std::array<uint64_t, 128> gapHistogram{};
            for (size_t id = 0; id < m_actors.size(); ++id)
            {
                const auto& actor = *m_actors[id];
                maxGap = (std::max)(maxGap, actor.m_maxGapMs);
                for (size_t bucket = 0; bucket < gapHistogram.size(); ++bucket)
                {
                    gapHistogram[bucket] += actor.m_gapHistogram[bucket];
                    gapSamples += actor.m_gapHistogram[bucket];
                }
                damage += actor.m_damage;
                healthChecksum ^= mix_value(actor.m_health + id * 131);
                if (m_verify)
                {
                    auto expected = m_expectedIds[id];
                    auto applied = actor.m_appliedIds;
                    std::sort(expected.begin(), expected.end());
                    std::sort(applied.begin(), applied.end());
                    if (expected != applied)
                        throw std::runtime_error("Event missing, duplicate or misdirected");
                    uint64_t receiverDamage = 0;
                    for (auto eventId : expected)
                        receiverDamage += 1 + mix_value(eventId) % 31;
                    if (actor.m_damage != receiverDamage)
                        throw std::runtime_error("Receiver damage mismatch");
                }
            }
            uint64_t expectedDamage = 0;
            for (uint64_t id = 1; id <= m_sent.load(); ++id)
                expectedDamage += 1 + mix_value(id) % 31;
            if (damage != expectedDamage || m_violations.load() != 0 || Task::statistics().live != 0)
                throw std::runtime_error("Damage, overlap or Task lifetime mismatch");
            print_late_traces();
            if (m_timerTraceMs != 0)
            {
                write_timer_profile(m_workers.get(), WORKERS, m_timerTraceMs);
            }
            const auto steadySeconds = (end - steadyBegin) / 1000.0;
            uint64_t p99Gap = 0, accumulatedGaps = 0;
            for (size_t bucket = 0; bucket < gapHistogram.size() && gapSamples != 0; ++bucket)
            {
                accumulatedGaps += gapHistogram[bucket];
                if (accumulatedGaps >= (gapSamples * 99 + 99) / 100)
                {
                    p99Gap = bucket + 1 == gapHistogram.size() ? maxGap : (bucket + 1) * 10;
                    break;
                }
            }
            std::printf("MOTION mode=%s workload=%s layout=%s actors=%zu seconds=%.3f seed=%u steady_fps=%.6f "
                        "cpu_seconds=%.6f moves=%llu sent=%llu applied=%llu damage=%llu overlaps=%llu "
                        "ownership_cycle=%d tasks_live=%llu health_checksum=%llu gap_samples=%llu "
                        "p99_gap_upper_ms=%llu max_gap_ms=%llu move_attempts=%zu settled_moves=%d\n",
                        mode, label, layout, m_actors.size(), (end - begin) / 1000.0, seed,
                        (updates - baseline) / (m_actors.size() * steadySeconds), cpu, m_moves, m_sent.load(),
                        m_applied.load(), damage, m_violations.load(), static_cast<int>(m_verify && m_areaMode),
                        Task::statistics().live, healthChecksum, gapSamples, p99Gap, maxGap, m_moveAttempts,
                        static_cast<int>(m_settledMoves));
        }
    };

    void ProbeActor::update()
    {
        if (m_updating.exchange(true))
        {
            ++m_world.m_violations;
            return;
        }

        auto area = m_world.m_areaMode ? currentProbeArea : m_expected.load();
        if (area == nullptr)
        {
            ++m_world.m_violations;
            m_updating.store(false);
            return;
        }
        m_observed.store(area);
        m_world.trace_actor_tick(*this, area);
        const auto updateTick = get_now_tick();
        if (m_world.m_measureGaps.load())
        {
            if (m_lastUpdateTick != 0)
            {
                const auto gap = static_cast<uint64_t>(updateTick - m_lastUpdateTick);
                m_maxGapMs = (std::max)(m_maxGapMs, gap);
                ++m_gapHistogram[(std::min)(m_gapHistogram.size() - 1, static_cast<size_t>(gap / 10))];
            }
            m_lastUpdateTick = updateTick;
        }
        else
        {
            m_lastUpdateTick = 0;
        }

        std::vector<ProbeEvent> events{};
        {
            std::lock_guard<std::mutex> guard(m_inboxLock);
            events.swap(m_inbox);
        }

        for (const auto& event : events)
        {
            m_damage += event.damage;
            m_health = event.damage >= m_health ? 1000 : m_health - event.damage;
            if (m_world.m_verify)
                m_appliedIds.push_back(event.id);
        }
        m_world.m_applied.fetch_add(events.size());

        const auto now = get_now_tick();
        if (m_world.m_combat.load() && now >= m_nextAttack)
        {
            m_nextAttack = now + 100;
            for (int candidate = 0; candidate < 16; ++candidate)
            {
                m_random = mix_value(m_random);
                const auto targetId = m_random % m_world.m_actors.size();
                auto target = m_world.m_actors[targetId]->m_observed.load();
                if (targetId != m_id && target != nullptr && std::abs(target->get_x() - area->get_x()) <= 2
                    && std::abs(target->get_y() - area->get_y()) <= 2)
                {
                    m_world.post(targetId);
                    break;
                }
            }
        }
        ++m_updates;
        m_updating.store(false);
    }
} // namespace

int run_motion_interaction_probe(int argc, char** argv)
{
    const std::string mode = argc > 2 ? argv[2] : "area";
    const int seconds = argc > 3 ? std::stoi(argv[3]) : 10;
    const size_t actors = argc > 4 ? std::stoull(argv[4]) : 5000;
    const uint32_t seed = argc > 5 ? static_cast<uint32_t>(std::stoul(argv[5])) : 42;
    const std::string workload = argc > 6 ? argv[6] : "verify";
    const std::string layout = argc > 7 ? argv[7] : "uniform";
    const std::string fault = argc > 8 ? argv[8] : "none";
    const size_t moveAttempts = argc > 9 ? std::stoull(argv[9]) : size_t{64};
    const int settledMoves = argc > 10 ? std::stoi(argv[10]) : 0;
    const int64_t traceMs = argc > 11 ? std::stoll(argv[11]) : int64_t{0};
    const uint64_t timerTraceMs = argc > 12 ? std::stoull(argv[12]) : (traceMs != 0 ? uint64_t{10} : uint64_t{0});
    if ((mode != "area" && mode != "job" && mode != "legacy") || seconds < 2 || seconds > 300 || actors == 0
        || actors > 70000 || (workload != "verify" && workload != "synthetic")
        || (layout != "uniform" && layout != "hotspot" && layout != "boundary")
        || (fault != "none" && fault != "event-id") || (fault != "none" && workload != "verify") || moveAttempts > 1024
        || (settledMoves != 0 && settledMoves != 1) || traceMs < 0 || traceMs > 1000
        || (traceMs != 0 && mode != "legacy") || timerTraceMs > 1000)
        throw std::invalid_argument("Invalid motion probe arguments");
    ProbeWorld world(mode, actors, seed, workload == "verify", layout == "hotspot", fault == "event-id",
                     layout == "boundary", moveAttempts, settledMoves == 1, traceMs, timerTraceMs);
    world.run(seconds, mode.c_str(), workload.c_str(), seed, layout.c_str());
    return 0;
}

namespace
{
    class DelayArea : public RadiusArea
    {
        std::function<void()> m_function{};

    public:
        explicit DelayArea(std::function<void()> function)
            : m_function(std::move(function))
        {
        }

    private:
        void do_update() override
        {
            m_function();
        }
    };

    class DelayTask : public Task
    {
        std::function<void()> m_function{};

    public:
        explicit DelayTask(std::function<void()> function)
            : m_function(std::move(function))
        {
        }
        void run() override
        {
            m_function();
        }
    };
} // namespace

int run_radius_delay_probe(int argc, char** argv)
{
    const int merge = argc > 2 ? std::stoi(argv[2]) : 1;
    const std::string phase = argc > 3 ? argv[3] : "timer";
    const int stallMs = argc > 4 ? std::stoi(argv[4]) : 100;
    const int repeats = argc > 5 ? std::stoi(argv[5]) : 5;
    const std::string load = argc > 6 ? argv[6] : "sleep";
    if ((merge != 0 && merge != 1) || (phase != "timer" && phase != "request" && phase != "area") || stallMs < 1
        || stallMs > 500 || repeats < 1 || repeats > 50 || (load != "sleep" && load != "busy"))
        throw std::invalid_argument("Invalid radius delay probe arguments");

    std::vector<double> delays{};
    size_t earlyUpdates = 0;
    for (int repeat = 0; repeat < repeats; ++repeat)
    {
        RadiusLocks locks(1);
        RadiusExecuter executor(1);
        executor.configure_publication(true, merge == 1);
        std::atomic_int64_t publishedNs{0}, delayNs{-1};
        std::atomic_bool returned{false}, early{false};
        DelayArea target(
            [&]
            {
                early.store(returned.load() == false);
                delayNs.store(motion_now_ns() - publishedNs.load());
            });
        target.set_footprint(locks.footprint({0}));
        const RadiusSubmission submission{&target, 0};
        auto publishAndBlock = [&]
        {
            publishedNs.store(motion_now_ns());
            executor.push_batch(&submission, 1);
            if (load == "sleep")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
            }
            else
            {
                const auto finish = motion_now_ns() + stallMs * int64_t{1000000};
                while (motion_now_ns() < finish)
                    std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            returned.store(true);
        };
        DelayArea publisher(publishAndBlock);
        publisher.set_footprint(locks.footprint({0}));
        Worker workers[2];
        for (size_t index = 0; index < 2; ++index)
            workers[index].start(&executor, index);
        const auto warmDeadline = get_now_tick() + 2000;
        while ((workers[0].statistics().signalWaits == 0 || workers[1].statistics().signalWaits == 0)
               && get_now_tick() < warmDeadline)
            std::this_thread::yield();
        const bool warmed = workers[0].statistics().signalWaits != 0 && workers[1].statistics().signalWaits != 0;
        if (phase == "area")
        {
            executor.push(&publisher, 0);
        }
        else if (phase == "timer")
        {
            workers[0].add_task(new DelayTask(publishAndBlock), 5);
        }
        else
        {
            workers[0].add_task(new DelayTask(publishAndBlock));
        }
        const auto deadline = get_now_tick() + stallMs + 3000;
        while ((delayNs.load() < 0 || returned.load() == false) && get_now_tick() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (auto& worker : workers)
            worker.stop();
        for (auto& worker : workers)
            worker.join();

        const auto expectedUpdates = phase == "area" ? uint64_t{2} : uint64_t{1};
        if (warmed == false || delayNs.load() < 0 || returned.load() == false || executor.updates() != expectedUpdates
            || executor.idle_state().readyWork != 0 || Task::statistics().live != 0)
            throw std::runtime_error("Radius delay probe failed to drain");
        delays.push_back(delayNs.load() / 1000000.0);
        earlyUpdates += early.load();
    }
    std::sort(delays.begin(), delays.end());
    std::printf("RADIUS_DELAY merge=%d phase=%s load=%s stall_ms=%d repeats=%d min_ms=%.3f median_ms=%.3f "
                "max_ms=%.3f early_updates=%zu tasks_live=%llu\n",
                merge, phase.c_str(), load.c_str(), stallMs, repeats, delays.front(), delays[delays.size() / 2],
                delays.back(), earlyUpdates, Task::statistics().live);
    return 0;
}

int run_handoff_chain_probe()
{
    class CountActor : public Actor
    {
    public:
        uint64_t count = 0;
        void update() override
        {
            ++count;
        }
    };

    AreaManager<ProbeArea> areas;
    areas.build(0, 0, 2, 1, 1, 1);
    auto first = areas.get_area(0, 0);
    auto second = areas.get_area(1, 0);
    CountActor moving{}, anchorFirst{}, anchorSecond{};
    first->add_object(&anchorFirst);
    second->add_object(&anchorSecond);
    first->add_object(&moving);
    first->update_at(1000);
    second->update_at(1000);
    const auto before = moving.count;
    const auto anchorsBefore = anchorFirst.count + anchorSecond.count;
    auto source = first;
    auto destination = second;
    for (size_t transfer = 0; transfer < 100; ++transfer)
    {
        if (source->move_object(&moving, destination) == false)
            throw std::runtime_error("Controlled transfer rejected");
        source->update_at(1010 + static_cast<int64_t>(transfer) * 10);
        std::swap(source, destination);
    }
    const auto during = moving.count - before;
    const auto anchorsDuring = anchorFirst.count + anchorSecond.count - anchorsBefore;
    source->update_at(2000);
    if (moving.count - before != during)
        throw std::runtime_error("Destination duplicated the source deadline update");
    source->update_at(2050);
    const auto resumed = moving.count - before - during;
    source->remove_object(&moving);
    source->update();
    first->remove_object(&anchorFirst);
    second->remove_object(&anchorSecond);
    first->update();
    second->update();
    if (during != 20 || resumed != 1 || first->is_active() || second->is_active())
        throw std::runtime_error("Controlled handoff did not settle and detach");

    std::printf("HANDOFF_CHAIN transfers=100 moving_updates=%llu anchor_updates=%llu resumed_updates=%llu "
                "detached=1\n",
                during, anchorsDuring, resumed);
    return 0;
}
