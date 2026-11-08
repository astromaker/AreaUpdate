#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Comparison
{
    struct Settings
    {
        size_t actors = 1000;
        size_t ticks = 30;
        size_t interactions = 8;
        size_t work = 100;
        size_t workers = 8;
        uint32_t seed = 42;
        std::string layout = "clustered";
        int periodMs = 0;
        size_t warmupTicks = 0;
        int mapSide = 320;
    };

    struct Layout
    {
        std::vector<std::vector<size_t>> cells;
        std::vector<std::pair<int, int>> positions;
    };

    inline Layout make_layout(const Settings& settings)
    {
        Layout layout{};
        layout.cells.resize(static_cast<size_t>(settings.mapSide) * settings.mapSide);
        std::mt19937 random(settings.seed);
        const int width = settings.layout == "uniform" ? settings.mapSide : settings.layout == "hotspot" ? 1 : 64;
        const int offset = (settings.mapSide - width) / 2;
        std::uniform_int_distribution<int> coordinate(offset, offset + width - 1);

        for (size_t id = 0; id < settings.actors; ++id)
        {
            const int x = coordinate(random), y = coordinate(random);
            layout.positions.emplace_back(x, y);
            layout.cells[static_cast<size_t>(y) * settings.mapSide + x].push_back(id);
        }

        return layout;
    }
} // namespace Comparison
