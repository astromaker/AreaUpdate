#pragma once

#include <cstdint>

struct UpdateTickPlan
{
    static constexpr int64_t INTERVAL_MS = 50;
    int64_t nextTick = 0;
    uint64_t skippedTicks = 0;
};

inline UpdateTickPlan next_update_tick(int64_t previousTick, int64_t now)
{
    UpdateTickPlan plan{};
    plan.nextTick = previousTick + UpdateTickPlan::INTERVAL_MS;
    if (plan.nextTick <= now)
    {
        plan.skippedTicks = static_cast<uint64_t>((now - plan.nextTick) / UpdateTickPlan::INTERVAL_MS + 1);
        plan.nextTick += static_cast<int64_t>(plan.skippedTicks) * UpdateTickPlan::INTERVAL_MS;
    }

    return plan;
}
