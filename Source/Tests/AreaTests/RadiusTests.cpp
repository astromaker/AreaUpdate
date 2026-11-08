#include "../../AreaUpdateLegacy/Optimized/RadiusExecuter.h"
#include "AreaManager.h"
#include <cstdio>
#include <stdexcept>

namespace
{
    struct RadiusState
    {
        std::array<std::atomic_int, 121> cells{};
        std::atomic_int violations{0}, completed{0};
        RadiusState()
        {
            for (auto& cell : cells)
                cell.store(0);
        }
    };
    void check_radius_at(bool value, int line)
    {
        if (value == false)
        {
            std::fprintf(stderr, "Radius regression failed at line %d\n", line);
            throw std::runtime_error("Radius regression failed");
        }
    }
#define check_radius(value) check_radius_at((value), __LINE__)

    class RadiusProbe : public RadiusArea
    {
    public:
        std::atomic_int updates{0};
        bool throws = false;
        RadiusProbe(int32_t, int32_t)
        {
        }

    private:
        void do_update() override
        {
            ++updates;
            if (throws)
                throw std::runtime_error("Injected radius update failure");
        }
    };

    void footprint_storage_regression()
    {
        RadiusLocks locks(64 * 40);
        RadiusProbe area(0, 0), blocker(0, 0), rollback(0, 0);
        for (size_t count : {size_t{1}, size_t{10}, size_t{11}, size_t{40}, size_t{1}})
        {
            std::vector<size_t> cells{};
            for (size_t index = 0; index < count; ++index)
                cells.push_back(index * 64);

            area.set_footprint(locks.footprint(cells));
            blocker.set_footprint(locks.footprint({cells.back()}));
            rollback.set_footprint(locks.footprint({0}));
            check_radius(blocker.try_acquire());
            check_radius(area.try_acquire() == false);
            if (count > 1)
            {
                check_radius(rollback.try_acquire());
                rollback.update();
            }

            blocker.update();
            check_radius(area.try_acquire());
            check_radius(blocker.try_acquire() == false);
            area.update();
            check_radius(blocker.try_acquire());
            blocker.update();
        }

        bool rejected = false;
        try
        {
            area.set_footprint({});
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        check_radius(rejected && area.try_acquire());
        area.update();
    }

    struct PublishGate
    {
        std::atomic_bool entered{false}, release{false}, timedOut{false};
    };

    void pause_publication(void* context)
    {
        auto& gate = *static_cast<PublishGate*>(context);
        if (gate.entered.exchange(true))
            return;

        const auto deadline = get_now_tick() + 3000;
        while (gate.release.load() == false && get_now_tick() < deadline)
            std::this_thread::yield();
        gate.timedOut.store(gate.release.load() == false);
    }

    template <class Predicate> bool await_radius(Predicate predicate)
    {
        const auto deadline = get_now_tick() + 2000;
        while (predicate() == false && get_now_tick() < deadline)
            std::this_thread::yield();
        return predicate();
    }

    void queue_trace_regression()
    {
        RadiusLocks locks(1);
        RadiusExecuter executor(1);
        RadiusProbe first(0, 0), second(0, 0), third(0, 0);
        first.set_footprint(locks.footprint({0}));
        second.set_footprint(locks.footprint({0}));
        third.set_footprint(locks.footprint({0}));
        executor.configure_queue_diagnostics(true);
        check_radius(executor.queue_trace(&first) == nullptr);
        executor.configure_queue_trace(&first, true);
        executor.configure_queue_trace(&second, true);
        executor.configure_queue_trace(&third, true);
        executor.push(&first, 0);
        executor.push(&second, 0);
        executor.push(&third, 0);
        Worker worker;
        worker.start(&executor, 0);
        const bool completed = await_radius(
            [&]
            {
                return third.updates.load() == 1;
            });
        worker.stop();
        worker.join();
        check_radius(completed && first.updates.load() == 1);

        const auto& trace = *executor.queue_trace(&second);
        check_radius(trace.publishedNs > 0 && trace.firstProbeNs >= trace.publishedNs);
        check_radius(trace.acquiredNs >= trace.firstProbeNs && trace.failures >= 1);
        check_radius(trace.attemptNs <= static_cast<uint64_t>(trace.acquiredNs - trace.firstProbeNs));
        const auto& following = *executor.queue_trace(&third);
        check_radius(following.holBeforeProbeNs > 0);
        check_radius(following.holBeforeProbeNs
                     <= static_cast<uint64_t>(following.firstProbeNs - following.publishedNs));
        executor.push(&second, 0);
        check_radius(executor.queue_trace(&second)->firstProbeNs == 0 && executor.queue_trace(&second)->failures == 0);
        Worker nextWorker;
        nextWorker.start(&executor, 0);
        const bool repeated = await_radius(
            [&]
            {
                return second.updates.load() == 2;
            });
        nextWorker.stop();
        nextWorker.join();
        check_radius(repeated && executor.queue_trace(&second)->failures == 0);
        executor.configure_queue_trace(&second, false);
        check_radius(executor.queue_trace(&second) == nullptr);
    }

    class OrderedRadiusProbe : public RadiusArea
    {
        std::vector<size_t>& m_order;
        size_t m_id = 0;

    public:
        OrderedRadiusProbe(std::vector<size_t>& order, size_t id)
            : m_order(order)
            , m_id(id)
        {
        }

    private:
        void do_update() override
        {
            m_order.push_back(m_id);
        }
    };

    void publication_wakeup(bool partial, bool fast, bool merge, bool homogeneous = false)
    {
        RadiusLocks locks(64);
        RadiusExecuter executor(4);
        executor.configure_publication(fast, merge);
        std::vector<size_t> order{};
        order.reserve(64);
        std::array<std::unique_ptr<OrderedRadiusProbe>, 64> areas{};
        std::array<RadiusSubmission, 64> submissions{};
        for (size_t index = 0; index < areas.size(); ++index)
        {
            areas[index] = std::make_unique<OrderedRadiusProbe>(order, index);
            areas[index]->set_footprint(locks.footprint({index}));
            submissions[index] = {areas[index].get(), homogeneous ? index * 4 + 3 : index + 4};
        }
        executor.push_batch(nullptr, 0);
        for (auto count : {size_t{1}, size_t{65}})
        {
            bool rejected = false;
            try
            {
                executor.push_batch(nullptr, count);
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            check_radius(rejected && executor.idle_state().readyWork == 0);
        }

        submissions[3].area = nullptr;
        bool rejectedNullArea = false;
        try
        {
            executor.push_batch(submissions.data(), submissions.size());
        }
        catch (const std::invalid_argument&)
        {
            rejectedNullArea = true;
        }
        check_radius(rejectedNullArea && executor.idle_state().readyWork == 0);
        submissions[3].area = areas[3].get();

        PublishGate waiting{}, published{};
        if (partial)
        {
            executor.set_group_hook(pause_publication, &published);
        }
        else
        {
            executor.set_publish_hook(pause_publication, &published);
        }
        Worker worker;
        worker.set_wait_hook(pause_publication, &waiting);
        auto waitForIdle = [&]
        {
            return await_radius(
                [&]
                {
                    return waiting.entered.load();
                });
        };
        auto waitForPublication = [&]
        {
            return await_radius(
                [&]
                {
                    return published.entered.load();
                });
        };
        auto publish = [&]
        {
            executor.push_batch(submissions.data(), submissions.size());
        };
        std::thread producer{};
        bool reachedWait = false, reachedPublication = false;
        if (partial)
        {
            producer = std::thread(publish);
            reachedPublication = waitForPublication();
            worker.start(&executor, 0);
            reachedWait = waitForIdle();
        }
        else
        {
            worker.start(&executor, 0);
            reachedWait = waitForIdle();
            producer = std::thread(publish);
            reachedPublication = waitForPublication();
        }

        waiting.release.store(true);
        const bool enteredSignalWait = await_radius(
            [&]
            {
                return worker.statistics().signalWaits != 0;
            });
        published.release.store(true);
        producer.join();
        const bool completed = await_radius(
            [&]
            {
                return executor.updates() == 64;
            });
        worker.stop();
        worker.join();
        check_radius(reachedWait && reachedPublication && enteredSignalWait && completed);
        check_radius(waiting.timedOut == false && published.timedOut == false);
        check_radius(executor.idle_state().readyWork == 0 && order.size() == 64);
        if (homogeneous)
        {
            for (size_t index = 0; index < 64; ++index)
                check_radius(order[index] == index);
        }
        else
        {
            for (size_t bucket = 0; bucket < 4; ++bucket)
            {
                for (size_t index = 0; index < 16; ++index)
                    check_radius(order[bucket * 16 + index] == bucket + index * 4);
            }
        }
    }

    void concurrent_publication(bool fast, bool merge)
    {
        RadiusLocks locks(64);
        RadiusExecuter executor(8);
        executor.configure_publication(fast, merge);
        std::array<std::unique_ptr<RadiusProbe>, 64> areas{};
        for (size_t index = 0; index < areas.size(); ++index)
        {
            areas[index] = std::make_unique<RadiusProbe>(0, 0);
            areas[index]->set_footprint(locks.footprint({index}));
        }
        Worker workers[4];
        for (size_t index = 0; index < 4; ++index)
            workers[index].start(&executor, index);

        std::atomic_bool failed{false};
        std::thread producers[2];
        for (size_t producer = 0; producer < 2; ++producer)
        {
            producers[producer] = std::thread(
                [&, producer]
                {
                    std::array<RadiusSubmission, 32> submissions{};
                    for (size_t index = 0; index < 32; ++index)
                        submissions[index] = {areas[producer * 32 + index].get(), index};
                    for (int epoch = 1; epoch <= 50; ++epoch)
                    {
                        executor.push_batch(submissions.data(), submissions.size());
                        const bool completed = await_radius(
                            [&]
                            {
                                for (size_t index = 0; index < 32; ++index)
                                {
                                    if (areas[producer * 32 + index]->updates.load() != epoch)
                                        return false;
                                }
                                return true;
                            });
                        if (completed == false)
                        {
                            failed.store(true);
                            break;
                        }
                    }
                });
        }
        for (auto& producer : producers)
            producer.join();
        for (auto& worker : workers)
            worker.stop();
        for (auto& worker : workers)
            worker.join();

        check_radius(failed == false && executor.updates() == 3200 && executor.idle_state().readyWork == 0);
        for (const auto& area : areas)
            check_radius(area->updates == 50);
    }

    class RadiusPublishTask : public Task
    {
        std::function<void()> m_function{};

    public:
        explicit RadiusPublishTask(std::function<void()> function)
            : m_function(std::move(function))
        {
        }
        void run() override
        {
            m_function();
        }
    };

    void publication_during_stop(bool fast)
    {
        RadiusLocks locks(128);
        RadiusExecuter executor(1);
        executor.configure(64, 1);
        executor.configure_publication(fast, true);
        std::array<std::unique_ptr<RadiusProbe>, 128> areas{};
        std::array<RadiusSubmission, 128> submissions{};
        for (size_t index = 0; index < 128; ++index)
        {
            areas[index] = std::make_unique<RadiusProbe>(0, 0);
            areas[index]->set_footprint(locks.footprint({index}));
            submissions[index] = {areas[index].get(), 0};
        }
        Worker workers[2];
        for (size_t index = 0; index < 2; ++index)
            workers[index].start(&executor, index);
        const bool waiting = await_radius(
            [&]
            {
                return workers[0].statistics().signalWaits != 0 && workers[1].statistics().signalWaits != 0;
            });
        workers[0].add_task(new RadiusPublishTask(
            [&]
            {
                executor.push_batch(submissions.data(), 64);
                executor.push_batch(submissions.data() + 64, 64);
                workers[0].stop();
            }));
        const bool completed = await_radius(
            [&]
            {
                return executor.updates() == 128;
            });
        for (auto& worker : workers)
            worker.stop();
        for (auto& worker : workers)
            worker.join();

        check_radius(waiting && completed && executor.idle_state().readyWork == 0);
        check_radius(executor.deferred_wakes() <= 2);
        for (const auto& area : areas)
            check_radius(area->updates == 1);
    }

    void first_publication_progress(bool timer, bool batch)
    {
        RadiusLocks locks(1);
        RadiusExecuter executor(1);
        executor.configure_publication(true, true);
        RadiusProbe area(0, 0);
        area.set_footprint(locks.footprint({0}));
        const RadiusSubmission submission{&area, 0};
        PublishGate callback{};
        std::atomic_bool returned{false};
        Worker workers[2];
        for (size_t index = 0; index < 2; ++index)
            workers[index].start(&executor, index);
        const bool waiting = await_radius(
            [&]
            {
                return workers[0].statistics().signalWaits != 0 && workers[1].statistics().signalWaits != 0;
            });
        auto task = new RadiusPublishTask(
            [&]
            {
                if (batch)
                {
                    executor.push_batch(&submission, 1);
                }
                else
                {
                    executor.push(&area, 0);
                }
                pause_publication(&callback);
                returned.store(true);
            });
        if (timer)
        {
            workers[0].add_task(task, 5);
        }
        else
        {
            workers[0].add_task(task);
        }
        const bool entered = await_radius(
            [&]
            {
                return callback.entered.load();
            });
        const bool independentProgress = await_radius(
            [&]
            {
                return area.updates.load() == 1;
            });
        const bool stillBlocked = returned.load() == false;
        callback.release.store(true);
        const bool completed = await_radius(
            [&]
            {
                return returned.load();
            });
        for (auto& worker : workers)
            worker.stop();
        for (auto& worker : workers)
            worker.join();

        check_radius(waiting && entered && independentProgress && stillBlocked && completed);
        check_radius(callback.timedOut == false && executor.updates() == 1 && executor.idle_state().readyWork == 0);
    }

    class ConcurrentRadiusProbe : public RadiusArea
    {
        RadiusExecuter& m_executor;
        RadiusState& m_state;
        std::vector<size_t> m_cells{};
        size_t m_bucket = 0;
        int m_remaining = 20;

    public:
        ConcurrentRadiusProbe(int32_t x, int32_t y, RadiusExecuter& executor, RadiusState& state)
            : m_executor(executor)
            , m_state(state)
            , m_bucket(static_cast<size_t>(y * 11 + x) % 4)
        {
            for (int ny = (std::max)(0, y - 2); ny <= (std::min)(10, y + 2); ++ny)
            {
                for (int nx = (std::max)(0, x - 2); nx <= (std::min)(10, x + 2); ++nx)
                    m_cells.push_back(static_cast<size_t>(ny * 11 + nx));
            }
        }

    private:
        void do_update() override
        {
            for (auto index : m_cells)
            {
                if (m_state.cells[index].fetch_add(1) != 0)
                    ++m_state.violations;
            }
            std::this_thread::yield();
            for (auto index : m_cells)
                --m_state.cells[index];

            if (--m_remaining != 0)
            {
                m_executor.push(this, m_bucket);
            }
            else
            {
                ++m_state.completed;
            }
        }
    };

    void concurrent_radius(size_t batch)
    {
        RadiusExecuter executor(4);
        std::atomic_size_t callbacks{0};
        executor.configure(batch, 16,
                           [&]
                           {
                               ++callbacks;
                           });
        RadiusState state{};
        RadiusLocks locks(121);
        AreaManager<ConcurrentRadiusProbe> grid;
        grid.build(0, 0, 11, 11, 1, 1, executor, state);
        grid.work(
            [&](int x, int y, ConcurrentRadiusProbe* area)
            {
                std::vector<size_t> cells{};
                for (int ny = (std::max)(0, y - 2); ny <= (std::min)(10, y + 2); ++ny)
                {
                    for (int nx = (std::max)(0, x - 2); nx <= (std::min)(10, x + 2); ++nx)
                        cells.push_back(static_cast<size_t>(ny * 11 + nx));
                }
                area->set_footprint(locks.footprint(std::move(cells)));
                executor.push(area, static_cast<size_t>(y * 11 + x) % 4);
            });
        Worker workers[4];
        for (size_t id = 0; id < 4; ++id)
        {
            workers[id].set_polling_wait(false);
            workers[id].start(&executor, id);
        }
        const auto deadline = get_now_tick() + 5000;
        while (state.completed != 121 && get_now_tick() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        for (auto& worker : workers)
            worker.stop();
        for (auto& worker : workers)
            worker.join();
        check_radius(state.completed == 121 && state.violations == 0);
        check_radius(executor.updates() == 121 * 20);
        check_radius(callbacks != 0);
        for (auto& cell : state.cells)
            check_radius(cell == 0);
    }
} // namespace

void test_radius()
{
    footprint_storage_regression();
    queue_trace_regression();
    for (auto fast : {false, true})
    {
        for (auto merge : {false, true})
        {
            publication_wakeup(false, fast, merge);
            publication_wakeup(true, fast, merge);
            publication_wakeup(false, fast, merge, true);
            concurrent_publication(fast, merge);
        }
        publication_during_stop(fast);
    }
    for (auto timer : {false, true})
    {
        for (auto batch : {false, true})
            first_publication_progress(timer, batch);
    }
    for (auto batch : {size_t{1}, size_t{4}, size_t{16}, size_t{64}})
        concurrent_radius(batch);

    for (auto batch : {size_t{0}, size_t{65}})
    {
        RadiusExecuter executor(1);
        bool rejected = false;
        try
        {
            executor.configure(batch, 0);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        check_radius(rejected);
    }

    RadiusLocks locks(121);
    AreaManager<RadiusProbe> grid;
    grid.build(0, 0, 11, 11, 1, 1);
    grid.work(
        [&](int x, int y, RadiusProbe* area)
        {
            std::vector<size_t> cells{};
            for (int ny = (std::max)(0, y - 2); ny <= (std::min)(10, y + 2); ++ny)
            {
                for (int nx = (std::max)(0, x - 2); nx <= (std::min)(10, x + 2); ++nx)
                    cells.push_back(static_cast<size_t>(ny * 11 + nx));
            }
            area->set_footprint(locks.footprint(std::move(cells)));
        });

    auto center = grid.get_area(2, 2);
    RadiusSample sample{};
    check_radius(center->try_acquire(&sample));
    center->update(&sample);
    check_radius(sample.attempts == 1 && sample.updates == 1);
    check_radius(center->try_acquire());
    for (int y = 0; y <= 6; ++y)
    {
        for (int x = 0; x <= 6; ++x)
            check_radius(grid.get_area(x, y)->try_acquire() == false);
    }
    auto distant = grid.get_area(7, 2);
    check_radius(distant->try_acquire());
    center->update();
    check_radius(distant->try_acquire() == false);
    distant->update();

    auto upper = grid.get_area(8, 8);
    check_radius(upper->try_acquire());
    check_radius(grid.get_area(4, 5)->try_acquire() == false);
    check_radius(center->try_acquire());
    center->update();
    upper->update();

    // Failed acquisitions must roll back every reference already acquired.
    grid.work(
        [&](int, int, RadiusProbe* area)
        {
            check_radius(area->try_acquire());
            area->update();
        });
    center->throws = true;
    check_radius(center->try_acquire());
    bool caught = false;
    try
    {
        center->update();
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }
    check_radius(caught);
    center->throws = false;
    check_radius(center->try_acquire());
    center->update();

    RadiusExecuter executor(2);
    const auto before = center->updates.load();
    executor.push(center, 0);
    Worker worker;
    worker.set_polling_wait(false);
    worker.start(&executor, 0);
    const auto deadline = get_now_tick() + 2000;
    while (center->updates.load() == before && get_now_tick() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    worker.stop();
    worker.join();
    check_radius(center->updates == before + 1 && executor.updates() == 1);
}
