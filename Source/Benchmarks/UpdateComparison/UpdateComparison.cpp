#include "TaskExecuter.h"
#include "AreaManager.h"
#include "MotionInteractionProbe.h"
#include "TaskQueue.h"
#include "Worker.h"
#include "OriginalAreaAdapter.h"
#include "ComparisonLayout.h"
#include "ComparisonTiming.h"
#include "UpdateTickPlan.h"
#include "../../Examples/AreaUpdateExample/RuntimeBackend.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr int RADIUS = 2;
    constexpr size_t PARTITION_WIDTH = 2 * RADIUS + 1;

    using Settings = Comparison::Settings;

    uint64_t step(uint64_t value)
    {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        return value;
    }

    class ComparisonWorld;

    class ComparisonActor : public Actor
    {
        friend class ComparisonWorld;
        ComparisonWorld& m_world;
        size_t m_id;
        uint64_t m_updates = 0;
        uint64_t m_damage = 0;
        uint64_t m_workValue;
        std::atomic_bool m_busy{false};
        std::shared_ptr<TaskExecuter> m_executer;
        std::vector<size_t> m_neighbors;

    public:
        ComparisonActor(ComparisonWorld& world, size_t id)
            : m_world(world)
            , m_id(id)
            , m_workValue(id + 1)
        {
        }
        void update() override;
        void apply_damage(uint64_t damage);
    };

    class ComparisonArea : public Area
    {
    public:
        ComparisonArea(int32_t x, int32_t y)
            : Area(x, y)
        {
        }

    private:
        void do_update() override
        {
        }
    };

    class OriginalComparisonArea : public Original::Area
    {
    public:
        std::vector<ComparisonActor*> actors;
        OriginalComparisonArea(int32_t, int32_t)
        {
        }

    private:
        void do_update() override
        {
            for (auto actor : actors)
                actor->update();
        }
    };

    struct Result
    {
        uint64_t checksum = 0;
        uint64_t events = 0;
        uint64_t violations = 0;
        double seconds = 0;
        double p99Ms = 0;
        size_t missedTicks = 0;
        double cpuSeconds = 0;
        uint64_t actualUpdates = 0;
    };

    class ComparisonWorld
    {
        Settings m_settings;
        std::string m_mode;
        TaskDispatcher& m_dispatcher;
        AreaManager<ComparisonArea> m_areas;
        AreaManager<OriginalComparisonArea> m_originalAreas;
        std::unique_ptr<Original::AreaExecuter> m_originalExecuter;
        std::vector<std::unique_ptr<ComparisonActor>> m_actors;
        std::vector<ComparisonArea*> m_partitions[PARTITION_WIDTH * PARTITION_WIDTH];
        std::atomic_uint64_t m_violations{0};
        size_t m_tick = 0;

        bool area_mode() const
        {
            return m_mode == "area" || m_mode == "area-runtime";
        }

    public:
        ComparisonWorld(Settings settings, std::string mode, TaskDispatcher& dispatcher)
            : m_settings(std::move(settings))
            , m_mode(std::move(mode))
            , m_dispatcher(dispatcher)
        {
            const auto layout = Comparison::make_layout(m_settings);

            build_areas();
            create_actors(layout);
            build_neighbors(layout);
            group_active_areas();
        }

    private:
        void build_areas()
        {
            const int mapSide = m_settings.mapSide;

            if (area_mode())
                m_areas.build(0, 0, static_cast<float>(mapSide), static_cast<float>(mapSide), 1, 1);
            if (m_mode == "original")
            {
                m_originalAreas.build(0, 0, static_cast<float>(mapSide), static_cast<float>(mapSide), 1, 1);
                m_originalExecuter = std::make_unique<Original::AreaExecuter>(m_settings.workers);

                m_originalAreas.work(
                    [&](int x, int y, OriginalComparisonArea* area)
                    {
                        for (int ny = (std::max)(0, y - RADIUS); ny <= (std::min)(mapSide - 1, y + RADIUS); ++ny)
                        {
                            for (int nx = (std::max)(0, x - RADIUS); nx <= (std::min)(mapSide - 1, x + RADIUS); ++nx)
                            {
                                if (nx != x || ny != y)
                                    area->add_ref(m_originalAreas.get_area(nx, ny));
                            }
                        }
                    });
            }
        }

        void create_actors(const Comparison::Layout& layout)
        {
            for (size_t id = 0; id < m_settings.actors; ++id)
            {
                const auto [x, y] = layout.positions[id];
                auto actor = std::make_unique<ComparisonActor>(*this, id);

                if (m_mode == "job")
                    actor->m_executer = std::make_shared<TaskExecuter>(m_dispatcher);
                if (area_mode())
                    m_areas.get_area(x, y)->add_object(actor.get());
                if (m_mode == "original")
                    m_originalAreas.get_area(x, y)->actors.push_back(actor.get());

                m_actors.emplace_back(std::move(actor));
            }
        }

        void build_neighbors(const Comparison::Layout& layout)
        {
            const int mapSide = m_settings.mapSide;

            for (size_t id = 0; id < m_actors.size(); ++id)
            {
                const auto [x, y] = layout.positions[id];
                auto& neighbors = m_actors[id]->m_neighbors;

                for (int ny = (std::max)(0, y - RADIUS); ny <= (std::min)(mapSide - 1, y + RADIUS); ++ny)
                {
                    for (int nx = (std::max)(0, x - RADIUS); nx <= (std::min)(mapSide - 1, x + RADIUS); ++nx)
                    {
                        for (auto other : layout.cells[ny * mapSide + nx])
                        {
                            if (other != id)
                                neighbors.push_back(other);
                        }
                    }
                }
            }
        }

        void group_active_areas()
        {
            if (area_mode())
            {
                m_areas.work(
                    [this](int, int, ComparisonArea* area)
                    {
                        if (area->is_active())
                            m_partitions[area->get_partition(PARTITION_WIDTH)].push_back(area);
                    });
            }
        }

    public:
        void violation()
        {
            ++m_violations;
        }

        void update_actor(ComparisonActor& actor)
        {
            const auto tick = m_mode == "area-runtime" ? actor.m_updates : m_tick;
            for (size_t i = 0; i < m_settings.work; ++i)
                actor.m_workValue = step(actor.m_workValue);

            ++actor.m_updates;
            if (actor.m_neighbors.empty())
                return;

            for (size_t event = 0; event < m_settings.interactions; ++event)
            {
                const auto index = step(actor.m_id + 1 + tick * 131 + event * 977) % actor.m_neighbors.size();
                auto target = m_actors[actor.m_neighbors[index]].get();
                const uint64_t damage = 1 + ((actor.m_id + tick + event) % 8);

                if (m_mode == "job")
                {
                    target->m_executer->run_task(new FunctionTask(
                        [target, damage]
                        {
                            target->apply_damage(damage);
                        }));
                }
                else
                {
                    target->apply_damage(damage);
                }
            }
        }

        Result run()
        {
            if (m_mode == "area-runtime")
                return run_runtime();
            if (m_mode == "original")
                Original::RUN_COUNT.store(0);

            const auto timing = Comparison::measure_ticks(m_settings.ticks, m_settings.periodMs,
                                                          [this](size_t tick)
                                                          {
                                                              m_tick = tick;
                                                              run_tick();
                                                          });
            m_tick = m_settings.ticks;

            Result result{};
            result.seconds = timing.seconds;
            result.cpuSeconds = timing.cpuSeconds;
            result.p99Ms = timing.p99Ms;
            result.missedTicks = timing.missedTicks;
            result.violations = m_violations.load();

            validate_result(result);
            return result;
        }

    private:
        Result run_runtime()
        {
            AreaExecuter executor(RADIUS);
            auto workers = std::make_unique<Worker[]>(m_settings.workers);
            for (const auto& partition : m_partitions)
            {
                for (auto area : partition)
                    executor.push(area);
            }

            const auto begin = Clock::now();
            const auto cpuBegin = Comparison::process_cpu_seconds();
            for (size_t id = 0; id < m_settings.workers; ++id)
                workers[id].start(&executor, id);
            std::this_thread::sleep_for(std::chrono::seconds(m_settings.ticks));
            for (size_t id = 0; id < m_settings.workers; ++id)
                workers[id].stop();
            for (size_t id = 0; id < m_settings.workers; ++id)
                workers[id].join();

            Result result{};
            result.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
            result.cpuSeconds = Comparison::process_cpu_seconds() - cpuBegin;
            result.violations = m_violations.load();
            validate_result(result);
            return result;
        }

        void run_task_tick()
        {
            for (auto& actor : m_actors)
            {
                auto pointer = actor.get();
                pointer->m_executer->run_task(new FunctionTask(
                    [pointer]
                    {
                        pointer->update();
                    }));
            }

            m_dispatcher.wait_idle();
        }

        void run_original_tick()
        {
            // Original had no Active Area filter: enqueue the entire map.
            m_originalAreas.work(
                [&](int, int, OriginalComparisonArea* area)
                {
                    m_originalExecuter->push(area);
                });

            for (size_t worker = 0; worker < m_settings.workers; ++worker)
            {
                m_dispatcher.post(
                    [this]
                    {
                        // A locked queue head can become runnable after another batch finishes.
                        while (Original::RUN_COUNT.load()
                               < static_cast<uint64_t>(m_settings.mapSide) * m_settings.mapSide * (m_tick + 1))
                        {
                            if (m_originalExecuter->run())
                                std::this_thread::yield();
                        }
                    });
            }

            m_dispatcher.wait_idle();
        }

        void run_area_tick()
        {
            constexpr size_t batchSize = 64;

            for (auto& partition : m_partitions)
            {
                for (size_t beginIndex = 0; beginIndex < partition.size(); beginIndex += batchSize)
                {
                    const auto endIndex = (std::min)(partition.size(), beginIndex + batchSize);
                    auto group = &partition;
                    const auto tick = 1000 + static_cast<int64_t>(m_tick) * UpdateTickPlan::INTERVAL_MS;

                    m_dispatcher.post(
                        [group, beginIndex, endIndex, tick]
                        {
                            for (size_t i = beginIndex; i < endIndex; ++i)
                                (*group)[i]->update_at(tick);
                        });
                }

                m_dispatcher.wait_idle();
            }
        }

        void run_tick()
        {
            if (m_mode == "job")
            {
                run_task_tick();
            }
            else if (m_mode == "original")
            {
                run_original_tick();
            }
            else if (m_mode == "area")
            {
                run_area_tick();
            }
            else
            {
                for (auto& actor : m_actors)
                    actor->update();
            }
        }

        void validate_result(Result& result) const
        {
            // Validate every receiver, not just the aggregate event count.
            std::vector<uint64_t> expected(m_actors.size(), 0);

            size_t ticks = m_settings.ticks;
            if (m_mode == "area-runtime")
            {
                for (const auto& actor : m_actors)
                    ticks = (std::max)(ticks, static_cast<size_t>(actor->m_updates));
            }
            for (size_t tick = 0; tick < ticks; ++tick)
            {
                for (const auto& actor : m_actors)
                {
                    if (m_mode == "area-runtime" && tick >= actor->m_updates)
                        continue;
                    if (actor->m_neighbors.empty() == false)
                    {
                        for (size_t event = 0; event < m_settings.interactions; ++event)
                        {
                            const auto index =
                                step(actor->m_id + 1 + tick * 131 + event * 977) % actor->m_neighbors.size();
                            expected[actor->m_neighbors[index]] += 1 + ((actor->m_id + tick + event) % 8);
                            ++result.events;
                        }
                    }
                }
            }

            for (const auto& actor : m_actors)
            {
                result.actualUpdates += actor->m_updates;
                if ((m_mode != "area-runtime" && actor->m_updates != m_settings.ticks) || actor->m_updates == 0
                    || actor->m_damage != expected[actor->m_id])
                    throw std::runtime_error("Actor state mismatch");

                result.checksum ^= step(actor->m_workValue + actor->m_damage + actor->m_id);
            }

            if (result.violations != 0)
                throw std::runtime_error("Concurrent Actor access detected");
        }
    };

    void ComparisonActor::update()
    {
        if (m_busy.exchange(true))
        {
            m_world.violation();
            return;
        }

        m_world.update_actor(*this);
        m_busy.store(false);
    }

    void ComparisonActor::apply_damage(uint64_t damage)
    {
        if (m_busy.exchange(true))
        {
            m_world.violation();
            return;
        }

        m_damage += damage;
        m_busy.store(false);
    }

    Result measure(const Settings& settings, const std::string& mode, TaskDispatcher& dispatcher)
    {
        if (settings.warmupTicks != 0)
        {
            auto warmup = settings;
            warmup.ticks = settings.warmupTicks;
            warmup.warmupTicks = 0;
            ComparisonWorld warmupWorld(warmup, mode, dispatcher);
            warmupWorld.run();
        }

        ComparisonWorld world(settings, mode, dispatcher);
        const auto result = world.run();
        const auto updates = static_cast<double>(result.actualUpdates);

        std::printf("mode=%s actors=%zu ticks=%zu interactions=%zu work=%zu workers=%zu layout=%s period_ms=%d "
                    "seconds=%.6f updates_per_sec=%.0f p99_tick_ms=%.3f missed_ticks=%zu events=%llu checksum=%llu "
                    "violations=%llu cpu_seconds=%.6f cpu_one_core_percent=%.3f warmup_ticks=%zu\n",
                    mode.c_str(), settings.actors, settings.ticks, settings.interactions, settings.work,
                    settings.workers, settings.layout.c_str(), settings.periodMs, result.seconds,
                    updates / result.seconds, result.p99Ms, result.missedTicks, result.events, result.checksum,
                    result.violations, result.cpuSeconds, result.cpuSeconds * 100 / result.seconds,
                    settings.warmupTicks);

        return result;
    }

    void verify(const Settings& settings, TaskDispatcher& dispatcher)
    {
        const auto reference = measure(settings, "single", dispatcher);

        for (const auto mode : {"job", "area", "original"})
        {
            const auto candidate = measure(settings, mode, dispatcher);

            if (reference.checksum != candidate.checksum || reference.events != candidate.events)
                throw std::runtime_error("Cross-mode checksum mismatch");
        }
    }

    void cpu_control(int seconds)
    {
        const size_t count = (std::max)(1u, std::thread::hardware_concurrency());
        std::atomic_size_t ready{0};
        std::atomic_bool start{false}, stop{false};
        std::vector<uint64_t> results(count, 0);
        std::vector<std::thread> threads;

        try
        {
            for (size_t id = 0; id < count; ++id)
            {
                threads.emplace_back(
                    [&, id]
                    {
                        uint64_t value = id + 1;
                        ++ready;
                        while (start.load() == false)
                            std::this_thread::yield();

                        while (stop.load(std::memory_order_relaxed) == false)
                        {
                            for (size_t i = 0; i < 4096; ++i)
                                value = step(value);
                        }
                        results[id] = value;
                    });
            }
        }
        catch (...)
        {
            stop = true;
            start = true;
            for (auto& thread : threads)
                thread.join();
            throw;
        }

        while (ready.load() != count)
            std::this_thread::yield();
        const auto cpuStart = Comparison::process_cpu_seconds();
        const auto begin = Clock::now();
        start = true;
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        stop = true;
        for (auto& thread : threads)
            thread.join();

        const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
        const auto cpuSeconds = Comparison::process_cpu_seconds() - cpuStart;
        uint64_t checksum = 0;
        for (const auto result : results)
            checksum ^= result;
        std::printf("CPU_CONTROL threads=%zu seconds=%.6f cpu_seconds=%.6f cpu_percent=%.3f checksum=%llu\n", count,
                    elapsed, cpuSeconds, 100 * cpuSeconds / elapsed / count, checksum);
    }

    void object_dispatch_trace_test()
    {
        ObjectDispatchStatistics first{};
        record_object_dispatch(first, 1500000, 250, 3);
        record_object_dispatch(first, 250000000, 400, 300);
        ObjectDispatchStatistics merged{};
        merge_object_dispatch(merged, first);
        std::array<size_t, 8> workerActors{};
        for (uint64_t tag = 1; tag <= 100000; ++tag)
        {
            if (sample_object_dispatch(tag, 256))
                ++workerActors[(tag - 1) % workerActors.size()];
        }

        if (merged.samples != 2 || merged.queueHistogram[1] != 1 || merged.queueHistogram[200] != 1
            || merged.timerHistogram[3] != 1 || merged.timerHistogram[200] != 1 || merged.actorNs != 650
            || merged.maxQueueNs != 250000000 || sample_object_dispatch(1, 0) || sample_object_dispatch(1, 1) == false)
        {
            throw std::runtime_error("Object dispatch trace aggregation failed");
        }
        for (const auto count : workerActors)
        {
            if (count == 0)
                throw std::runtime_error("Object samples missed a timer Worker");
        }
        std::puts("OBJECT_DISPATCH_TRACE_TEST passed");
    }

    void object_runtime_test()
    {
        class RuntimeProbeArea final : public Area
        {
        public:
            RuntimeProbeArea(int32_t x, int32_t y)
                : Area(x, y)
            {
            }
            void do_update() override
            {
            }
        };
        class RuntimeProbeActor final : public Actor
        {
            std::atomic_size_t m_count{0}, m_overlaps{0};
            std::atomic_bool m_updating{false};

        public:
            void update() override
            {
                if (m_updating.exchange(true))
                {
                    ++m_overlaps;
                    return;
                }

                ++m_count;
                m_updating = false;
            }
            size_t count() const
            {
                return m_count.load();
            }
            size_t overlaps() const
            {
                return m_overlaps.load();
            }
        };

        const auto before = Task::statistics();
        AreaManager<RuntimeProbeArea> areas;
        areas.build(0, 0, 4, 4, 1, 1);
        AreaExecuter executor(2);
        Worker workers[2];
        RuntimeProbeActor actor;
        RuntimeBackend<RuntimeProbeArea> backend("job", executor, workers, 2, 2, 1, 1);
        backend.prepare(areas);
        workers[0].start(nullptr, 0);
        workers[1].start(nullptr, 1);
        backend.add_object(&actor, areas.get_area(0, 0));
        bool rejectedLiveRead = false;
        try
        {
            backend.dispatch_profile();
        }
        catch (const std::logic_error&)
        {
            rejectedLiveRead = true;
        }
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (actor.count() < 3 && Clock::now() < deadline)
            std::this_thread::yield();

        backend.stop();
        for (auto& worker : workers)
            worker.stop();
        for (auto& worker : workers)
            worker.join();
        backend.finish();

        const auto after = Task::statistics();
        const auto& profile = backend.dispatch_profile();
        const auto& dispatcherStatistics = backend.dispatcher_statistics();
        uint64_t dispatcherTasks = 0, executerTasks = 0, dispatcherSamples = 0;
        for (const auto& worker : dispatcherStatistics)
        {
            dispatcherTasks += worker.dispatchTasks;
            executerTasks += worker.executerTasks;
            dispatcherSamples += worker.samples;
        }
        if (actor.count() < 3 || actor.overlaps() != 0 || after.live != before.live || rejectedLiveRead == false
            || profile.selectedActors != 1 || profile.statistics.samples != actor.count() - 1
            || profile.workerSamples[0] != profile.statistics.samples || profile.workerSamples[1] != 0
            || dispatcherStatistics.size() != 2 || executerTasks < actor.count()
            || dispatcherTasks != dispatcherSamples)
            throw std::runtime_error("Object runtime execution or shutdown failed");
        for (const auto& worker : dispatcherStatistics)
        {
            if (worker.executerTasks == 0)
                throw std::runtime_error("Actual Dispatcher Worker statistics missing");
        }
        std::puts("OBJECT_RUNTIME_TEST passed");
    }

    void dispatcher_statistics_test()
    {
        TaskDispatcher dispatcher(4, 8);
        bool rejectedLiveRead = false;
        try
        {
            dispatcher.worker_statistics();
        }
        catch (const std::logic_error&)
        {
            rejectedLiveRead = true;
        }

        std::atomic_size_t completed{0};
        std::vector<std::thread> producers;
        for (size_t i = 0; i < 4; ++i)
        {
            producers.emplace_back(
                [&]
                {
                    for (size_t task = 0; task < 256; ++task)
                    {
                        auto executer = std::make_shared<TaskExecuter>(dispatcher);
                        executer->run_task(new FunctionTask(
                            [&]
                            {
                                ++completed;
                            }));
                    }
                });
        }
        for (auto& producer : producers)
            producer.join();
        dispatcher.shutdown();
        dispatcher.shutdown();
        const auto statistics = dispatcher.worker_statistics();
        if (rejectedLiveRead == false || completed.load() != 1024 || statistics.size() != 4)
            throw std::runtime_error("Dispatcher statistics lifecycle failed");
        for (const auto& worker : statistics)
        {
            if (worker.dispatchTasks != 256 || worker.executerTasks != 256 || worker.samples != 32
                || worker.busyNs < worker.sampledRunNs || worker.elapsedNs < worker.busyNs + worker.idleNs)
            {
                throw std::runtime_error("Dispatcher Worker counts or sampling failed");
            }
        }

        std::atomic_size_t destroyed{0};
        class RejectedTask final : public Task
        {
            std::atomic_size_t& m_destroyed;

        public:
            explicit RejectedTask(std::atomic_size_t& counter)
                : m_destroyed(counter)
            {
            }
            ~RejectedTask() override
            {
                ++m_destroyed;
            }
            void run() override
            {
            }
        };
        bool rejectedPost = false, rejectedExecute = false;
        try
        {
            dispatcher.post(std::make_unique<RejectedTask>(destroyed));
        }
        catch (const std::logic_error&)
        {
            rejectedPost = true;
        }
        auto stoppedExecuter = std::make_shared<TaskExecuter>(dispatcher);
        try
        {
            stoppedExecuter->run_task(new RejectedTask(destroyed));
        }
        catch (const std::logic_error&)
        {
            rejectedExecute = true;
        }
        if (rejectedPost == false || rejectedExecute == false || destroyed.load() != 2)
            throw std::runtime_error("Dispatcher shutdown ownership failed");

        TaskDispatcher unprofiled(1);
        unprofiled.post([] {});
        unprofiled.shutdown();
        if (unprofiled.worker_statistics()[0].dispatchTasks != 0)
            throw std::runtime_error("Disabled Dispatcher profiling recorded tasks");
        std::puts("DISPATCHER_STATISTICS_TEST passed");
    }

    void dispatcher_completion_test()
    {
        struct CompletionGate
        {
            std::atomic_bool ran{false}, destroying{false}, release{false}, waiting{false}, returned{false};
        } gate;
        class CompletionTask final : public Task
        {
            CompletionGate& m_gate;

        public:
            explicit CompletionTask(CompletionGate& value)
                : m_gate(value)
            {
            }
            ~CompletionTask() override
            {
                m_gate.destroying = true;
                while (m_gate.release.load() == false)
                    std::this_thread::yield();
            }
            void run() override
            {
                m_gate.ran = true;
            }
        };

        TaskDispatcher dispatcher(2);
        dispatcher.post(std::make_unique<CompletionTask>(gate));
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (gate.destroying.load() == false && Clock::now() < deadline)
            std::this_thread::yield();

        std::thread waiter(
            [&]
            {
                gate.waiting = true;
                dispatcher.wait_idle();
                gate.returned = true;
            });
        while (gate.waiting.load() == false && Clock::now() < deadline)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool waitedForDestruction = gate.destroying.load() && gate.returned.load() == false;
        gate.release = true;
        waiter.join();

        bool rejectedNull = false;
        try
        {
            dispatcher.post(std::unique_ptr<Task>{});
        }
        catch (const std::invalid_argument&)
        {
            rejectedNull = true;
        }

        std::atomic_int completed{0};
        auto executer = std::make_shared<TaskExecuter>(dispatcher);
        std::weak_ptr<TaskExecuter> weak = executer;
        executer->run_task(new FunctionTask(
            [&]
            {
                ++completed;
            }));
        executer.reset();
        dispatcher.wait_idle();

        if (gate.ran.load() == false || waitedForDestruction == false || gate.returned.load() == false
            || rejectedNull == false || completed.load() != 1 || weak.expired() == false)
        {
            throw std::runtime_error("Dispatcher ownership or completion failed");
        }
        std::puts("DISPATCHER_COMPLETION_TEST passed");
    }

    void dispatcher_mixed_queue_test()
    {
        TaskDispatcher dispatcher(1);
        std::atomic_bool entered{false}, release{false};
        std::vector<size_t> order{};
        dispatcher.post(
            [&]
            {
                entered = true;
                while (release.load() == false)
                    std::this_thread::yield();
            });
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (entered.load() == false && Clock::now() < deadline)
            std::this_thread::yield();

        for (size_t index = 0; index < 256; ++index)
        {
            auto callback = [&, index]
            {
                order.push_back(index);
            };
            if (index % 2 == 0)
            {
                auto executer = std::make_shared<TaskExecuter>(dispatcher);
                executer->run_task(new FunctionTask(callback));
            }
            else
            {
                dispatcher.post(callback);
            }
        }
        release = true;
        dispatcher.wait_idle();

        if (entered.load() == false || order.size() != 256)
            throw std::runtime_error("Mixed Dispatcher queue lost work");
        for (size_t index = 0; index < order.size(); ++index)
        {
            if (order[index] != index)
                throw std::runtime_error("Mixed Dispatcher queue lost FIFO order");
        }
        std::puts("DISPATCHER_MIXED_QUEUE_TEST passed");
    }

    void unified_task_test()
    {
        const auto before = Task::statistics();
        TaskQueue queue;
        TaskQueue secondary;
        std::vector<size_t> order;

        queue.push(new FunctionTask(
            [&]
            {
                order.push_back(0);
            }));
        secondary.push(queue.pop_all());
        for (size_t i = 1; i < 128; ++i)
        {
            secondary.push(new FunctionTask(
                [&, i]
                {
                    order.push_back(i);
                }));
        }

        auto task = secondary.pop_all();
        while (task != nullptr)
        {
            auto next = TaskQueue::next(task);
            task->run();
            delete task;
            task = next;
        }

        if (order.size() != 128 || queue.empty() == false || secondary.pop_all() != nullptr)
            throw std::runtime_error("Unified Task queue drain failed");
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (order[i] != i)
                throw std::runtime_error("TaskQueue FIFO failed");
        }

        std::atomic_int completed{0};
        {
            TaskDispatcher dispatcher(2);
            auto executer = std::make_shared<TaskExecuter>(dispatcher);
            executer->run_task(new FunctionTask(
                [&]
                {
                    ++completed;
                }));
            dispatcher.wait_idle();
        }

        Worker worker;
        worker.start(nullptr, 0);
        const bool immediateAccepted = worker.add_task(new FunctionTask(
            [&]
            {
                ++completed;
            }));
        const bool timerAccepted = worker.add_task(new FunctionTask(
                                                       [&]
                                                       {
                                                           ++completed;
                                                       }),
                                                   10);

        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (completed.load() < 3 && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        worker.stop();
        worker.join();

        const auto after = Task::statistics();
        if (immediateAccepted == false || timerAccepted == false || completed.load() != 3 || after.live != before.live
            || after.created - before.created != after.destroyed - before.destroyed)
        {
            throw std::runtime_error("Unified Task execution or lifetime failed");
        }
        std::puts("UNIFIED_TASK_TEST passed");
    }

    void self_test()
    {
        object_dispatch_trace_test();
        object_runtime_test();
        dispatcher_statistics_test();
        dispatcher_completion_test();
        dispatcher_mixed_queue_test();
        unified_task_test();

        {
            TaskDispatcher balance(4, 1);
            std::atomic_bool entered{false}, release{false}, queuedRan{false};
            std::atomic_int otherCompleted{0};

            balance.post(
                [&]
                {
                    entered = true;
                    while (release.load() == false)
                        std::this_thread::yield();
                });

            for (int i = 0; i < 3; ++i)
            {
                balance.post(
                    [&]
                    {
                        ++otherCompleted;
                    });
            }
            // Round-robin returns to the Worker held by the first task.
            balance.post(
                [&]
                {
                    queuedRan = true;
                });

            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while ((entered.load() == false || otherCompleted.load() != 3) && Clock::now() < deadline)
                std::this_thread::yield();
            const bool reproduced = entered.load() && otherCompleted.load() == 3 && queuedRan.load() == false;

            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            release = true;
            balance.shutdown();
            const auto statistics = balance.worker_statistics();

            if (reproduced == false || queuedRan.load() == false)
                throw std::runtime_error("Load imbalance reproduction failed");
            if (statistics[0].dispatchTasks != 2 || statistics[0].samples != 2 || statistics[0].maxQueueDelayMs < 50
                || statistics[0].busyNs < 50000000)
            {
                throw std::runtime_error("Busy Worker imbalance was not measured");
            }
            for (size_t id = 1; id < statistics.size(); ++id)
            {
                if (statistics[id].dispatchTasks != 1 || statistics[id].samples != 1)
                    throw std::runtime_error("Round-robin Worker counts changed");
            }
            for (size_t id = 0; id < statistics.size(); ++id)
            {
                const auto& worker = statistics[id];
                std::printf("IMBALANCE_WORKER id=%zu dispatch_tasks=%llu busy_ms=%.3f queue_max_ms=%llu\n", id,
                            worker.dispatchTasks, worker.busyNs / 1000000.0, worker.maxQueueDelayMs);
            }
            std::puts("LOAD_BALANCE_TEST: 3 Workers completed; queued task waited for busy Worker");
        }

        TaskDispatcher dispatcher(4);
        auto executer = std::make_shared<TaskExecuter>(dispatcher);
        uint64_t count = 0;
        std::vector<std::thread> producers;

        for (int producer = 0; producer < 4; ++producer)
        {
            producers.emplace_back(
                [&]
                {
                    for (int i = 0; i < 1000; ++i)
                    {
                        executer->run_task(new FunctionTask(
                            [&]
                            {
                                ++count;
                            }));
                    }
                });
        }

        for (auto& producer : producers)
            producer.join();
        dispatcher.wait_idle();

        if (count != 4000)
            throw std::runtime_error("Mailbox serialization failed");

        Settings settings{};
        settings.actors = 256;
        settings.ticks = 3;
        settings.workers = 4;

        for (const auto layout : {"uniform", "clustered", "hotspot"})
        {
            settings.layout = layout;
            verify(settings, dispatcher);
        }

        settings.actors = 1;
        verify(settings, dispatcher);
        std::puts("SELF_TEST passed");
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--motion-probe")
            return run_motion_interaction_probe(argc, argv);
        if (argc > 1 && std::string(argv[1]) == "--radius-delay-probe")
            return run_radius_delay_probe(argc, argv);
        if (argc > 1 && std::string(argv[1]) == "--handoff-chain-probe")
            return run_handoff_chain_probe();
        if (argc > 1 && std::string(argv[1]) == "--cpu-control")
        {
            const int seconds = argc > 2 ? std::stoi(argv[2]) : 10;
            if (seconds <= 0 || seconds > 120)
                throw std::invalid_argument("CPU control duration must be between 1 and 120 seconds");
            cpu_control(seconds);
            return 0;
        }

        if (argc > 1 && std::string(argv[1]) == "--test")
        {
            self_test();
            return 0;
        }

        const std::string mode = argc > 1 ? argv[1] : "all";
        Settings settings{};

        if (argc > 2)
            settings.actors = std::stoull(argv[2]);
        if (argc > 3)
            settings.ticks = std::stoull(argv[3]);
        if (argc > 4)
            settings.interactions = std::stoull(argv[4]);
        if (argc > 5)
            settings.work = std::stoull(argv[5]);
        if (argc > 6)
            settings.workers = std::stoull(argv[6]);
        if (argc > 7)
            settings.seed = static_cast<uint32_t>(std::stoul(argv[7]));
        if (argc > 8)
            settings.layout = argv[8];
        if (argc > 9)
            settings.periodMs = std::stoi(argv[9]);
        if (argc > 10)
            settings.warmupTicks = std::stoull(argv[10]);
        if (argc > 11)
            settings.mapSide = std::stoi(argv[11]);

        if (settings.mapSide < 64 || settings.mapSide > 2048)
            throw std::invalid_argument("mapSide must be between 64 and 2048");
        if (settings.actors == 0 || settings.ticks == 0 || settings.workers == 0 || settings.periodMs < 0)
            throw std::invalid_argument("actors, ticks, workers must be positive; period must be nonnegative");
        if (mode == "area-runtime" && settings.ticks > 120)
            throw std::invalid_argument("area-runtime uses ticks as seconds, maximum 120");
        if (mode != "all" && mode != "job" && mode != "area" && mode != "single" && mode != "original"
            && mode != "area-runtime")
            throw std::invalid_argument("mode: all/job/area/original/single");
        if (settings.layout != "uniform" && settings.layout != "clustered" && settings.layout != "hotspot")
            throw std::invalid_argument("layout: uniform/clustered/hotspot");

        TaskDispatcher dispatcher(settings.workers);
        if (mode == "all")
        {
            verify(settings, dispatcher);
        }
        else
        {
            measure(settings, mode, dispatcher);
        }

        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
