#include "Area.h"
#include <cstdlib>
#include <cstdio>
#include "AreaExecuter.h"
#include <thread>
#include <chrono>
#include <memory>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <algorithm>
#include "AreaManager.h"

static void check_at(bool value, int line)
{
    if (value == false)
    {
        std::fprintf(stderr, "Check failed at line %d\n", line);
        std::abort();
    }
}
#define check(value) check_at((value), __LINE__)
void test_runtime();
void test_radius();

struct TestActor : Actor
{
    int updates = 0;
    int64_t servicedDeadline = 0;
    void update() override
    {
        servicedDeadline = get_update_deadline();
        ++updates;
    }
};

struct TestArea : Area
{
    using Area::Area;
    void do_update() override
    {
    }
};

struct RescheduleProbe : Area
{
    using Area::Area;
    std::atomic_int updates{0};

    void do_update() override
    {
        if (++updates == 3)
            set_active(false);
    }
};

static void test_actor_deadlines()
{
    TestArea source(0, 0), target(1, 0);
    TestActor actor{}, anchor{};
    source.add_object(&actor);
    target.add_object(&anchor);
    source.update_at(1000);
    target.update_at(1020);
    check(actor.servicedDeadline == 0 && actor.get_update_deadline() == 1050);
    check(source.next_actor_tick(1100) == 1050);

    check(source.move_object(&actor, &target));
    source.update_at(1030);
    target.update_at(1040);
    check(actor.updates == 1);
    check(target.next_actor_tick(1070) == 1050);
    target.update_at(1050);
    target.update_at(1050);
    check(actor.updates == 2 && anchor.updates == 1);
    check(actor.servicedDeadline == 1050 && actor.get_update_deadline() == 1100);

    check(target.move_object(&actor, &source));
    target.update_at(1210);
    check(actor.updates == 3);
    check(actor.servicedDeadline == 1100 && actor.get_update_deadline() == 1250);
    source.update_at(1210);
    check(actor.updates == 3);
    check(source.next_actor_tick(1300) == 1250);
    source.update_at(1250);
    check(actor.updates == 4);

    source.remove_object(&actor);
    source.update_at(1300);
    check(actor.updates == 4 && source.is_active() == false);
    target.remove_object(&anchor);
    target.update_at(1300);
    check(target.is_active() == false);
}

static void test_active_update_paths()
{
    struct ActiveProbe : Area
    {
        using Area::Area;
        int updates = 0;

        void do_update() override
        {
            ++updates;
            set_active(false);
        }
    };

    ActiveProbe area(0, 0);
    TestActor first{}, second{};
    area.update_at(1000);
    check(area.updates == 0);

    area.activate();
    area.update_at(1000);
    area.update_at(1050);
    check(area.updates == 1 && area.is_active() == false);

    area.add_object(&first);
    area.update_at(1100);
    area.add_object(&second);
    area.update_at(1120);
    check(area.updates == 3 && first.updates == 1 && second.updates == 1);
    check(area.next_actor_tick(1200) == 1150);

    area.remove_object(&first);
    area.update_at(1150);
    check(first.updates == 1 && second.updates == 1);
    check(area.next_actor_tick(1200) == 1170);

    area.remove_object(&second);
    area.update_at(1200);
    check(area.updates == 4 && area.is_active() == false);
}

static void test_reservation_batches()
{
    for (size_t count : {1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 64, 65})
    {
        const auto before = Task::statistics();
        AreaExecuter executor(2);
        std::vector<std::unique_ptr<RescheduleProbe>> areas{};
        for (size_t index = 0; index < count; ++index)
        {
            areas.push_back(std::make_unique<RescheduleProbe>(static_cast<int32_t>(index * 5), 0));
            areas.back()->activate();
            executor.push(areas.back().get());
        }
        Worker worker;
        worker.start(&executor, 0);

        const auto completed = [&areas]
        {
            for (const auto& area : areas)
            {
                if (area->updates < 3)
                    return false;
            }
            return true;
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (completed() == false && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        worker.stop();
        worker.join();

        for (const auto& area : areas)
        {
            check(area->updates == 3);
            check(area->is_active() == false);
        }
        const auto after = Task::statistics();
        check(after.live == before.live);
        check(after.created - before.created == after.destroyed - before.destroyed);
    }
}

struct ConstructionState
{
    int attempts = 0;
    int live = 0;
    int failAt = -1;
};

struct alignas(64) StorageProbe
{
    ConstructionState* state;
    char padding[64 - sizeof(ConstructionState*)]{};
    StorageProbe(int32_t, int32_t, ConstructionState& value)
        : state(&value)
    {
        if (++state->attempts == state->failAt)
            throw std::runtime_error("Construction probe");
        ++state->live;
    }
    ~StorageProbe()
    {
        --state->live;
    }
};

static void test_area_storage()
{
    static_assert(std::is_copy_constructible_v<AreaManager<StorageProbe>> == false);
    static_assert(std::is_copy_assignable_v<AreaManager<StorageProbe>> == false);
    ConstructionState state{};

    {
        AreaManager<StorageProbe> manager;
        manager.build(0, 0, 3, 2, 1, 1, state);

        auto first = manager.get_area(0, 0);
        check(state.live == 6);
        check(manager.get_count_x() == 3 && manager.get_count_y() == 2);
        check(manager.get_area(1, 0) == first + 1 && manager.get_area(0, 1) == first + 3);
        check(reinterpret_cast<uintptr_t>(first) % alignof(StorageProbe) == 0);

        state.failAt = state.attempts + 3;
        bool failed = false;
        try
        {
            manager.build(0, 0, 4, 4, 1, 1, state);
        }
        catch (const std::runtime_error&)
        {
            failed = true;
        }
        check(failed && state.live == 6 && manager.get_area(0, 0) == first);
        check(manager.get_count_x() == 3 && manager.get_count_y() == 2);

        failed = false;
        try
        {
            manager.build(0, 0, 2, 2, 0, 1, state);
        }
        catch (const std::invalid_argument&)
        {
            failed = true;
        }
        check(failed && state.live == 6 && manager.get_area(0, 0) == first);

        state.failAt = -1;
        manager.build(0, 0, 2, 1, 1, 1, state);
        check(state.live == 2 && manager.get_count_x() == 2 && manager.get_count_y() == 1);
        check(manager.select(0, 0, 2).size() == 2);
    }
    check(state.live == 0);

    ConstructionState failure{0, 0, 2};
    AreaManager<StorageProbe> empty;
    bool failed = false;
    try
    {
        empty.build(0, 0, 3, 1, 1, 1, failure);
    }
    catch (const std::runtime_error&)
    {
        failed = true;
    }
    check(failed && failure.live == 0 && empty.get_count_x() == 0 && empty.get_area(0, 0) == nullptr);
}

static void test_area_queue()
{
    TestArea areas[] = {{0, 0}, {5, 0}, {10, 0}, {15, 0}, {20, 0}};
    AreaQueue queue;
    check(queue.pop_all(1).empty() && queue.empty());

    for (auto& area : areas)
        queue.push(&area);

    check(queue.size() == 5);
    const auto single = queue.pop_all(1);
    check(single.size() == 1 && single[0] == &areas[0]);

    const auto first = queue.pop_all(2);
    check(first.size() == 2 && first[0] == &areas[1] && first[1] == &areas[2]);

    const auto rest = queue.pop_all();
    check(rest.size() == 2 && rest[0] == &areas[3] && rest[1] == &areas[4]);
    check(queue.empty() && queue.size() == 0 && queue.pop_all().empty());
}

int main()
{
    test_actor_deadlines();
    test_active_update_paths();
    test_runtime();
    test_radius();
    test_area_storage();
    test_area_queue();
    test_reservation_batches();

    // Radius r footprints are disjoint with a (2r + 1)-wide coloring.
    for (int radius : {1, 2})
    {
        const int width = 2 * radius + 1;
        for (int origin : {-10, -1, 0, 10})
        {
            TestArea center(origin, origin);
            for (int dy = -2 * radius; dy <= 2 * radius; ++dy)
            {
                for (int dx = -2 * radius; dx <= 2 * radius; ++dx)
                {
                    if (dx != 0 || dy != 0)
                    {
                        TestArea other(origin + dx, origin + dy);
                        check(center.get_partition(width) != other.get_partition(width));
                    }
                }
            }
            TestArea next(origin + width, origin);
            check(center.get_partition(width) == next.get_partition(width));
        }
    }

    TestArea negative(-1, -1), minimum((std::numeric_limits<int32_t>::min)(), 0);
    check(negative.get_partition(5) == 24);
    check(negative.get_partition(1) == 0);
    check(negative.get_partition(0) == 0);
    check(minimum.get_partition(5) == 2);

    // Re-registration must refresh the cached index for a different partition radius.
    TestArea cached(-1, -1);
    for (size_t radius : {size_t{1}, size_t{2}, size_t{0}, size_t{2}})
    {
        AreaExecuter cacheExecutor(radius);
        const auto expected = cacheExecutor.get_queue_index(&cached);
        cached.activate();
        cacheExecutor.push(&cached);
        cacheExecutor.push(&cached);
        Worker cacheWorker;
        cacheWorker.start(&cacheExecutor, 0);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cacheExecutor.partition_areas()[expected] < 3 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        cached.set_active(false);
        cacheWorker.stop();
        cacheWorker.join();

        const auto counts = cacheExecutor.partition_areas();
        check(counts[expected] >= 3);
        for (size_t index = 0; index < counts.size(); ++index)
        {
            if (index != expected)
                check(counts[index] == 0);
        }
        check(cached.finish_schedule() == false);
    }

    TestActor actor;
    {
        TestArea crowded(0, 0), destination(1, 0);
        std::vector<std::unique_ptr<TestActor>> actors;
        for (int i = 0; i < 512; ++i)
        {
            actors.push_back(std::make_unique<TestActor>());
            crowded.add_object(actors.back().get());
        }

        crowded.update_at(1000);
        for (size_t i = 0; i < actors.size(); i += 2)
            check(crowded.move_object(actors[i].get(), &destination));

        crowded.update_at(1050);
        destination.update_at(1050);
        for (auto& value : actors)
            check(value->updates == 2);
        for (size_t i = 0; i < actors.size(); ++i)
            (i % 2 ? crowded : destination).remove_object(actors[i].get());

        crowded.update();
        destination.update();
        check(crowded.is_active() == false && destination.is_active() == false);
    }

    TestArea source(0, 0), target(1, 0);
    source.add_object(&actor);
    source.add_object(&actor);
    source.update_at(1000);
    check(actor.updates == 1);

    source.move_object(&actor, &target);
    target.update_at(1025);
    check(actor.updates == 1);

    source.update_at(1050);
    check(source.is_active() == false && target.is_active());
    target.update_at(1050);
    check(actor.updates == 2);

    target.remove_object(&actor);
    target.remove_object(&actor);
    target.update();
    check(target.is_active() == false);

    target.add_object(&actor);
    target.update();
    check(actor.updates == 3);

    source.activate();
    check(source.request_schedule());
    check(source.request_schedule() == false);
    source.update();
    check(source.finish_schedule());
    check(source.request_schedule() == false);

    source.set_active(false);
    check(source.finish_schedule() == false);

    for (int y = -4; y <= 4; ++y)
    {
        for (int x = -4; x <= 4; ++x)
        {
            if (x != 0 || y != 0)
            {
                TestArea neighbor(x + 5, y + 5);
                TestArea center(5, 5);
                check(neighbor.get_partition(5) != center.get_partition(5));
            }
        }
    }

    int wakes = 0;
    TestArea eventArea(0, 0);
    eventArea.set_wake(
        [&](Area*)
        {
            ++wakes;
        });
    eventArea.activate();
    check(wakes == 1);

    struct MovingActor : Actor
    {
        Area* source = nullptr;
        Actor* other = nullptr;
        std::atomic_int* arrived = nullptr;
        void update() override
        {
            arrived->fetch_add(1);
            while (arrived->load() != 2)
                std::this_thread::yield();

            source->remove_object(other);
        }
    };
    TestArea left(0, 0), right(5, 0);
    MovingActor a, b;
    std::atomic_int arrived{0};

    a.source = &right;
    a.other = &b;
    a.arrived = &arrived;
    b.source = &left;
    b.other = &a;
    b.arrived = &arrived;

    left.add_object(&a);
    right.add_object(&b);
    std::thread first(
        [&]
        {
            left.update();
        });
    std::thread second(
        [&]
        {
            right.update();
        });
    first.join();
    second.join();

    for (int i = 0; i < 200; ++i)
    {
        TestActor transferred;
        TestArea from(0, 0), to(1, 0), thief(2, 0);
        from.add_object(&transferred);
        check(from.move_object(&transferred, &to));
        std::thread contender(
            [&]
            {
                for (int j = 0; j < 100; ++j)
                    thief.add_object(&transferred);
            });
        from.update_at(1000);
        contender.join();

        check(to.is_active() && thief.is_active() == false);
        to.update_at(1000);
        check(transferred.updates == 1);
    }

    struct CountingArea : Area
    {
        std::atomic_int count{0};
        CountingArea()
            : Area(0, 0)
        {
        }
        void do_update() override
        {
            ++count;
        }
    } counting;
    AreaExecuter executor(2);
    Worker workers[2];
    counting.set_wake(
        [&](Area* area)
        {
            executor.push(area);
        });
    workers[0].start(&executor, 0);
    workers[1].start(&executor, 1);
    counting.activate();

    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < end)
    {
        executor.push(&counting);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    workers[0].stop();
    workers[1].stop();
    workers[0].join();
    workers[1].join();

    check(counting.count > 0 && counting.count <= 13);

    struct SlowArea : Area
    {
        std::atomic_int& m_started;
        SlowArea(int x, std::atomic_int& started)
            : Area(x, 0)
            , m_started(started)
        {
        }
        void do_update() override
        {
            ++m_started;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
            while (std::chrono::steady_clock::now() < until)
            {
            }
        }
    };
    AreaSchedulingOptions fairOptions{};
    fairOptions.unlimited = false;
    AreaExecuter fairExecutor(2, fairOptions);
    fairExecutor.configure_profile(1, 3, get_now_tick());
    std::atomic_int started{0};
    std::vector<std::unique_ptr<SlowArea>> busy;
    // All busy areas are partition 0; cold is placed in partition 1 below.
    struct ColdArea : Area
    {
        std::atomic_int count{0};
        std::atomic_int observed{0};
        std::atomic_int& m_started;
        explicit ColdArea(std::atomic_int& started)
            : Area(1, 0)
            , m_started(started)
        {
        }
        void do_update() override
        {
            if (count.load() == 0)
                observed = m_started.load();
            ++count;
        }
    } coldArea(started);

    for (int i = 0; i < 1024; ++i)
    {
        busy.emplace_back(std::make_unique<SlowArea>(i * 5, started));
        busy.back()->activate();
        fairExecutor.push(busy.back().get());
    }

    coldArea.activate();
    fairExecutor.push(&coldArea);
    Worker fairWorkers[2];
    fairWorkers[0].start(&fairExecutor, 0);
    fairWorkers[1].start(&fairExecutor, 1);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (coldArea.count == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    fairWorkers[0].stop();
    fairWorkers[1].stop();
    fairWorkers[0].join();
    fairWorkers[1].join();

    check(coldArea.count > 0);
    check(coldArea.observed > 0 && coldArea.observed <= 8 * 64);
    check(fairExecutor.partition_areas()[1] > 0);
    const auto fairProfile = fairExecutor.profile_partitions();
    check(fairProfile[0].budgetBacklogEpochs > 0 && fairProfile[0].admissions >= 8);
    check(fairProfile[0].areas >= 512 && fairProfile[0].maxRemainingAreas >= 512);
}
