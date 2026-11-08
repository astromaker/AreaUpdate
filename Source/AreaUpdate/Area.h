
#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <utility>

#ifndef AREAUPDATE_RUN_COUNTER
#define AREAUPDATE_RUN_COUNTER 0
#endif

#if AREAUPDATE_RUN_COUNTER != 0 && AREAUPDATE_RUN_COUNTER != 1
#error AREAUPDATE_RUN_COUNTER must be 0 or 1
#endif

#if AREAUPDATE_RUN_COUNTER
extern std::atomic_uint64_t areaRunCount;
#endif

class Area;

class Actor
{
    friend class Area;
    std::mutex m_updateLock;
    Area* m_owner = nullptr;
    Area* m_destination = nullptr;
    bool m_leaving = false;
    // Owned by the updating Area; transferred without resetting the Actor's cadence.
    int64_t m_nextUpdateTick = 0;

public:
    virtual ~Actor() = default;

    virtual void update() = 0;
    // Owner-thread only. During update(), this is the deadline being serviced (0 on the first update).
    int64_t get_update_deadline() const
    {
        return m_nextUpdateTick;
    }
};

class Area
{
    friend class AreaExecuter;
    struct PendingActorChanges
    {
        std::vector<Actor*> adds;
        std::vector<Actor*> removes;
    };
    std::atomic_bool m_active{false};
    std::function<void(Area*)> m_wake;
    // Includes pending ownership reservations, not only entries in m_actors.
    std::atomic_int32_t m_actorCount{0};
    std::vector<Actor*> m_actors;
    std::mutex m_pendingLock;
    std::vector<Actor*> m_pendingAdds;
    std::vector<Actor*> m_pendingRemoves;
    std::atomic_bool m_pending{false};
    std::mutex m_scheduleLock;
    bool m_scheduled = false;
    int64_t m_nextUpdateTick = 0;
    size_t m_scheduledPartition = 0;
    int64_t m_nextActorTick = 0;
    int32_t m_x = 0;
    int32_t m_y = 0;

public:
    Area(int32_t x, int32_t y);
    virtual ~Area() = default;

    int32_t get_x() const
    {
        return m_x;
    }
    int32_t get_y() const
    {
        return m_y;
    }
    size_t get_partition(size_t partitionWidth) const;

    void activate();
    void set_wake(std::function<void(Area*)> wake)
    {
        m_wake = std::move(wake);
    }
    void set_active(bool active);
    void add_object(Actor* actor);
    void remove_object(Actor* actor);
    bool move_object(Actor* actor, Area* destination);
    bool request_schedule();
    bool finish_schedule();
    // Read the deadline only on the Area owner or after Workers have joined.
    int64_t get_scheduled_tick() const
    {
        return m_nextUpdateTick;
    }
    bool is_active() const;

    void update();
    // Same ownership contract as update(); now is a monotonic millisecond clock (also used by deterministic tests).
    void update_at(int64_t now);
    // Read only from the Area scheduler owner after update().
    int64_t next_actor_tick(int64_t fallback) const;
    // Read capacities only after workers have joined (or from the Area owner).
    size_t retained_actor_slots() const
    {
        return m_actors.capacity() + m_pendingAdds.capacity() + m_pendingRemoves.capacity();
    }

private:
    void notify_scheduler();
    PendingActorChanges take_pending_changes();
    void apply_membership_changes(const PendingActorChanges& changes);
    void complete_transfers(const std::vector<Actor*>& removes);
    void update_actor(Actor* actor, int64_t now);

    virtual void do_update() = 0;
};
