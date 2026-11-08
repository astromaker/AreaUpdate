#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Comparison
{
    struct Timing
    {
        double seconds = 0;
        double cpuSeconds = 0;
        double p99Ms = 0;
        size_t missedTicks = 0;
    };

    inline double process_cpu_seconds()
    {
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};

        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE)
            throw std::runtime_error("GetProcessTimes failed");

        const auto value = [](FILETIME time)
        {
            return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };

        return (value(kernel) + value(user)) / 10000000.0;
    }

    template <class Function> Timing measure_ticks(size_t ticks, int periodMs, Function runTick)
    {
        using Clock = std::chrono::steady_clock;
        Timing result{};
        std::vector<double> durations;

        const auto cpuStart = process_cpu_seconds();
        auto nextTick = Clock::now();
        const auto start = nextTick;

        for (size_t tick = 0; tick < ticks; ++tick)
        {
            const auto begin = Clock::now();

            runTick(tick);

            const auto finish = Clock::now();
            durations.push_back(std::chrono::duration<double, std::milli>(finish - begin).count());

            if (periodMs > 0)
            {
                nextTick += std::chrono::milliseconds(periodMs);
                if (finish > nextTick)
                {
                    ++result.missedTicks;
                    nextTick = finish;
                }
                else
                {
                    std::this_thread::sleep_until(nextTick);
                }
            }
        }

        result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        result.cpuSeconds = process_cpu_seconds() - cpuStart;

        std::sort(durations.begin(), durations.end());
        result.p99Ms = durations[(durations.size() * 99 + 99) / 100 - 1];

        return result;
    }
} // namespace Comparison
