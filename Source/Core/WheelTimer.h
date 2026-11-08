#pragma once

#include "PriorityTimer.h"
#include <array>
#include <algorithm>

// Worker-owned short timer wheel plus overflow heap, adapted from PWTimer.
class WheelTimer
{
    static constexpr int64_t SLOT_MS = 5;
    static constexpr size_t SLOT_COUNT = 256;
    struct Slot
    {
        Task* head = nullptr;
        Task* tail = nullptr;
    };
    std::array<Slot, SLOT_COUNT> m_slots{};
    PriorityTimer m_overflow;
    int64_t m_cursor;
    int64_t m_nextExpire = (std::numeric_limits<int64_t>::max)();
    size_t m_count = 0;

    static int64_t slot_for(int64_t tick)
    {
        return tick / SLOT_MS + (tick % SLOT_MS != 0);
    }
    void refresh_deadline()
    {
        m_nextExpire = m_overflow.next_expire_tick();

        for (const auto& slot : m_slots)
        {
            if (slot.head != nullptr)
                m_nextExpire = (std::min)(m_nextExpire, slot.head->m_wheelTick);
        }
    }

public:
    static constexpr int64_t resolution_ms()
    {
        return SLOT_MS;
    }
    static constexpr size_t slot_count()
    {
        return SLOT_COUNT;
    }

    explicit WheelTimer(int64_t now = get_now_tick())
        : m_cursor(now / SLOT_MS)
    {
    }
    ~WheelTimer()
    {
        clear();
    }
    WheelTimer(const WheelTimer&) = delete;
    WheelTimer& operator=(const WheelTimer&) = delete;

    void push_at(Task* task, int64_t expire)
    {
        task->m_expireTick = expire;
        const auto slot = (std::max)(m_cursor, slot_for(expire));

        if (slot - m_cursor < static_cast<int64_t>(SLOT_COUNT))
        {
            task->m_wheelTick = slot * SLOT_MS;
            auto& bucket = m_slots[static_cast<size_t>(slot) % SLOT_COUNT];
            task->m_next = nullptr;

            if (bucket.tail != nullptr)
            {
                bucket.tail->m_next = task;
            }
            else
            {
                bucket.head = task;
            }

            bucket.tail = task;
            m_nextExpire = (std::min)(m_nextExpire, slot * SLOT_MS);
        }
        else
        {
            task->m_wheelTick = expire;
            m_overflow.push_at(task, expire);
            m_nextExpire = (std::min)(m_nextExpire, expire);
        }

        ++m_count;
    }

    std::vector<Task*> expired_tasks(int64_t now, size_t limit = 256)
    {
        if (m_count == 0)
        {
            m_cursor = now / SLOT_MS;
            return {};
        }

        auto result = m_overflow.expired_tasks(now, (limit + 1) / 2);
        const auto target = now / SLOT_MS;
        size_t scanned = 0;

        while (m_cursor <= target && scanned < SLOT_COUNT && result.size() < limit)
        {
            auto& slot = m_slots[static_cast<size_t>(m_cursor) % SLOT_COUNT];
            while (slot.head != nullptr && result.size() < limit)
            {
                result.push_back(slot.head);
                slot.head = slot.head->m_next;
                if (slot.head == nullptr)
                    slot.tail = nullptr;
            }

            if (slot.head != nullptr)
                break;
            ++m_cursor;
            ++scanned;
        }

        // A long stall cannot require walking millions of empty elapsed slots.
        if (scanned == SLOT_COUNT && m_cursor <= target)
            m_cursor = target + 1;

        if (result.size() < limit)
        {
            auto extra = m_overflow.expired_tasks(now, limit - result.size());
            result.insert(result.end(), extra.begin(), extra.end());
        }

        m_count -= result.size();
        if (result.empty() == false)
            refresh_deadline();

        return result;
    }

    int64_t next_expire_tick() const
    {
        return m_nextExpire;
    }
    size_t size() const
    {
        return m_count;
    }
    size_t retained_bytes() const
    {
        return m_overflow.retained_bytes();
    }
    void clear()
    {
        for (auto& slot : m_slots)
        {
            while (slot.head != nullptr)
            {
                auto next = slot.head->m_next;
                delete slot.head;
                slot.head = next;
            }
            slot.tail = nullptr;
        }

        m_overflow.clear();
        m_count = 0;
        m_nextExpire = (std::numeric_limits<int64_t>::max)();
    }
};
