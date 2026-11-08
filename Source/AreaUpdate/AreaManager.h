#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "Area.h"

template <class AreaType> class AreaManager
{
    class Storage
    {
        using Allocator = std::allocator<AreaType>;
        using Traits = std::allocator_traits<Allocator>;
        Allocator m_allocator;
        AreaType* m_data;
        size_t m_capacity;
        size_t m_constructed = 0;

    public:
        explicit Storage(size_t count)
            : m_data(Traits::allocate(m_allocator, count))
            , m_capacity(count)
        {
        }
        Storage(const Storage&) = delete;
        Storage& operator=(const Storage&) = delete;
        ~Storage()
        {
            for (size_t i = 0; i < m_constructed; ++i)
                Traits::destroy(m_allocator, m_data + i);

            Traits::deallocate(m_allocator, m_data, m_capacity);
        }
        template <class... Args> void emplace(Args&&... args)
        {
            Traits::construct(m_allocator, m_data + m_constructed, std::forward<Args>(args)...);

            ++m_constructed;
        }
        AreaType* get(size_t index) const
        {
            return m_data + index;
        }
    };

    std::unique_ptr<Storage> m_storage;
    int32_t m_countX = 0;
    int32_t m_countY = 0;
    float m_minX = 0;
    float m_minY = 0;
    int32_t m_areaXSize = 0;
    int32_t m_areaYSize = 0;

    static int32_t count_cells(float minimum, float maximum, int32_t cellSize)
    {
        if (std::isfinite(minimum) == false || std::isfinite(maximum) == false || maximum <= minimum || cellSize <= 0)
            throw std::invalid_argument("Invalid Area grid dimensions");

        const double count = std::ceil(std::ceil(static_cast<double>(maximum) - minimum) / cellSize);
        if (count > (std::numeric_limits<int32_t>::max)())
            throw std::length_error("Area grid dimension overflow");

        return static_cast<int32_t>(count);
    }

public:
    AreaManager() = default;
    AreaManager(const AreaManager&) = delete;
    AreaManager& operator=(const AreaManager&) = delete;

    int32_t get_count_x() const
    {
        return m_countX;
    }
    int32_t get_count_y() const
    {
        return m_countY;
    }

    // Build/rebuild only while workers are stopped; success invalidates old Area pointers.
    template <class... Args>
    void build(float minX, float minY, float maxX, float maxY, int32_t areaXSize, int32_t areaYSize, Args&&... args)
    {
        const int32_t countX = count_cells(minX, maxX, areaXSize);
        const int32_t countY = count_cells(minY, maxY, areaYSize);
        const size_t width = static_cast<size_t>(countX);
        const size_t height = static_cast<size_t>(countY);

        if (height > (std::numeric_limits<size_t>::max)() / width / sizeof(AreaType))
            throw std::length_error("Area grid allocation overflow");

        auto next = std::make_unique<Storage>(width * height);
        for (int32_t y = 0; y < countY; ++y)
        {
            for (int32_t x = 0; x < countX; ++x)
                next->emplace(x, y, std::forward<Args>(args)...);
        }

        m_storage.swap(next);
        m_countX = countX;
        m_countY = countY;
        m_minX = minX;
        m_minY = minY;
        m_areaXSize = areaXSize;
        m_areaYSize = areaYSize;
    }

    template <class Function> void work(Function function)
    {
        for (int32_t y = 0; y < m_countY; ++y)
        {
            for (int32_t x = 0; x < m_countX; ++x)
                function(x, y, get_area(x, y));
        }
    }

    AreaType* get_area(int32_t x, int32_t y) const
    {
        if (x < 0 || x >= m_countX || y < 0 || y >= m_countY)
            return nullptr;

        return m_storage->get(static_cast<size_t>(y) * m_countX + x);
    }

    std::vector<AreaType*> select(int32_t x, int32_t y, int32_t radius) const
    {
        std::vector<AreaType*> areas;

        if (radius <= 0)
        {
            auto area = get_area(x, y);

            if (area != nullptr)
                areas.push_back(area);

            return areas;
        }

        const auto minX = (std::max)(static_cast<int64_t>(x) - radius, int64_t{0});
        const auto minY = (std::max)(static_cast<int64_t>(y) - radius, int64_t{0});
        const auto maxX = (std::min)(static_cast<int64_t>(x) + radius, static_cast<int64_t>(m_countX) - 1);
        const auto maxY = (std::min)(static_cast<int64_t>(y) + radius, static_cast<int64_t>(m_countY) - 1);

        for (auto row = minY; row <= maxY; ++row)
        {
            for (auto column = minX; column <= maxX; ++column)
                areas.push_back(get_area(static_cast<int32_t>(column), static_cast<int32_t>(row)));
        }

        return areas;
    }

    int32_t get_x(float x) const
    {
        return static_cast<int32_t>((x - m_minX) / m_areaXSize);
    }
    int32_t get_y(float y) const
    {
        return static_cast<int32_t>((y - m_minY) / m_areaYSize);
    }
    AreaType* get_area(float x, float y) const
    {
        return get_area(get_x(x), get_y(y));
    }
    std::vector<AreaType*> select(float x, float y, int32_t radius) const
    {
        return select(get_x(x), get_y(y), radius);
    }
};
