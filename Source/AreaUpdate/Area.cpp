
#include "Area.h"
#include "UpdateTickPlan.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

#if AREAUPDATE_RUN_COUNTER
std::atomic_uint64_t areaRunCount{0};
#endif

Area::Area(int32_t x, int32_t y)
    : m_x(x)
    , m_y(y)
{
}

size_t Area::get_partition(size_t partitionWidth) const
{
    if (partitionWidth == 0)
        return 0;

    const auto modulo = [partitionWidth](int32_t coordinate)
    {
        const int64_t value = coordinate;

        if (value >= 0)
            return static_cast<size_t>(value) % partitionWidth;

        const size_t remainder = static_cast<size_t>(-value) % partitionWidth;
        return remainder == 0 ? size_t{0} : partitionWidth - remainder;
    };

    size_t x = modulo(m_x);
    size_t y = modulo(m_y);

    return (y * partitionWidth) + x;
}

void Area::activate()
{
    set_active(true);
}

void Area::notify_scheduler()
{
    if (m_wake)
        m_wake(this);
}

void Area::set_active(bool active)
{
    m_active.store(active, std::memory_order_relaxed);

    if (active)
        notify_scheduler();
}

void Area::add_object(Actor* actor)
{
    if (actor == nullptr)
        return;

    {
        std::lock_guard<std::mutex> actorGuard(actor->m_updateLock);
        std::lock_guard<std::mutex> guard(m_pendingLock);

        if (actor->m_owner != nullptr)
            return;

        actor->m_owner = this;
        actor->m_nextUpdateTick = 0;
        m_pendingAdds.emplace_back(actor);
        m_actorCount.fetch_add(1);
        m_pending.store(true);
    }

    notify_scheduler();
}

void Area::remove_object(Actor* actor)
{
    move_object(actor, nullptr);
}

bool Area::move_object(Actor* actor, Area* destination)
{
    if (actor == nullptr || destination == this)
        return false;

    {
        std::lock_guard<std::mutex> actorGuard(actor->m_updateLock);
        std::lock_guard<std::mutex> guard(m_pendingLock);

        if (actor->m_owner != this || actor->m_leaving)
            return false;

        actor->m_destination = destination;
        actor->m_leaving = true;
        m_pendingRemoves.push_back(actor);
        m_pending.store(true);
    }

    notify_scheduler();
    return true;
}

bool Area::request_schedule()
{
    std::lock_guard<std::mutex> guard(m_scheduleLock);

    if (m_scheduled || is_active() == false)
        return false;

    m_scheduled = true;
    return true;
}

bool Area::finish_schedule()
{
    std::lock_guard<std::mutex> guard(m_scheduleLock);

    if (is_active())
        return true;

    m_scheduled = false;
    return false;
}

bool Area::is_active() const
{
    return m_active.load(std::memory_order_relaxed) || m_actorCount.load(std::memory_order_relaxed) > 0
           || m_pending.load();
}

void Area::update()
{
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    update_at(now);
}

int64_t Area::next_actor_tick(int64_t fallback) const
{
    return m_nextActorTick == 0 ? fallback : (std::min)(fallback, m_nextActorTick);
}

void Area::update_actor(Actor* actor, int64_t now)
{
    if (actor->m_nextUpdateTick != 0 && now < actor->m_nextUpdateTick)
        return;

    const auto previous = actor->m_nextUpdateTick == 0 ? now : actor->m_nextUpdateTick;
    actor->update();
    actor->m_nextUpdateTick = next_update_tick(previous, now).nextTick;
}

void Area::update_at(int64_t now)
{
    if (is_active() == false)
        return;

    PendingActorChanges changes{};
    if (m_pending.load())
    {
        changes = take_pending_changes();
        apply_membership_changes(changes);
    }

    m_nextActorTick = 0;
    // Only the Area update removes residents; a nonempty list keeps the Area active.
    if (m_actors.empty())
    {
        bool transferring = false;
        for (auto actor : changes.removes)
            transferring = transferring || actor->m_destination != nullptr;

        if (is_active() == false && transferring == false)
        {
            complete_transfers(changes.removes);
            return;
        }
    }

    do_update();

    int64_t nextActorTick = 0;
    for (auto actor : m_actors)
    {
        if (actor != nullptr)
        {
            update_actor(actor, now);
            const auto actorTick = actor->m_nextUpdateTick;
            if (nextActorTick == 0 || actorTick < nextActorTick)
                nextActorTick = actorTick;
        }
    }

    m_nextActorTick = nextActorTick;

    // Service an overdue Actor under the source Area's spatial protection before publishing ownership.
    for (auto actor : changes.removes)
    {
        if (actor->m_destination != nullptr)
            update_actor(actor, now);
    }
    complete_transfers(changes.removes);

#if AREAUPDATE_RUN_COUNTER
    ++areaRunCount;
#endif
}

Area::PendingActorChanges Area::take_pending_changes()
{
    PendingActorChanges changes{};

    std::lock_guard<std::mutex> guard(m_pendingLock);

    changes.adds.swap(m_pendingAdds);
    changes.removes.swap(m_pendingRemoves);
    m_pending.store(false);

    return changes;
}

void Area::apply_membership_changes(const PendingActorChanges& changes)
{
    const auto& adds = changes.adds;
    const auto& removes = changes.removes;

    // Ownership prevents duplicate registration; preserve Actor iteration order.
    m_actors.insert(m_actors.end(), adds.begin(), adds.end());

    if (removes.size() == 1)
    {
        const auto end = std::remove(m_actors.begin(), m_actors.end(), removes.front());
        m_actorCount.fetch_sub(static_cast<int32_t>(m_actors.end() - end));
        m_actors.erase(end, m_actors.end());
    }
    else if (removes.empty() == false)
    {
        const std::unordered_set<Actor*> leaving(removes.begin(), removes.end());
        const auto end = std::remove_if(m_actors.begin(), m_actors.end(),
                                        [&](Actor* actor)
                                        {
                                            return leaving.find(actor) != leaving.end();
                                        });

        m_actorCount.fetch_sub(static_cast<int32_t>(m_actors.end() - end));
        m_actors.erase(end, m_actors.end());
    }
}

void Area::complete_transfers(const std::vector<Actor*>& removes)
{
    for (auto actor : removes)
    {
        Area* destination = nullptr;

        {
            std::lock_guard<std::mutex> guard(actor->m_updateLock);
            destination = actor->m_destination;

            if (destination != nullptr)
            {
                std::lock_guard<std::mutex> targetGuard(destination->m_pendingLock);
                destination->m_pendingAdds.push_back(actor);
                destination->m_actorCount.fetch_add(1);
                destination->m_pending.store(true);
            }

            actor->m_owner = destination;
            actor->m_destination = nullptr;
            actor->m_leaving = false;
        }

        if (destination != nullptr)
            destination->notify_scheduler();
    }
}
