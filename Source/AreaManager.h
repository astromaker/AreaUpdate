
#pragma once

#include <cstdint>
#include <cmath>

#include <vector>

#include "Area.h"



template<class AreaType>
class AreaManager
{
	enum
	{
		LINK_RADIUS = 2,
	};

	AreaType* m_areas = nullptr;

	int32_t m_countX = 0;
	int32_t m_countY = 0;

	float m_minX = 0;
	float m_minY = 0;
	float m_maxX = 0;
	float m_maxY = 0;

	int32_t m_areaXSize = 0;
	int32_t m_areaYSize = 0;

public:

	AreaManager() = default;
	~AreaManager()
	{
		clear();
	}

	int32_t getCountX() const { return m_countX; }
	int32_t getCountY() const { return m_countY; }

	template<typename... Args>
	void build(float minX, float minY, float maxX, float maxY, int32_t areaXSize, int32_t areaYSize, Args&&... args)
	{
		m_minX = minX;
		m_minY = minY;
		m_maxX = maxX;
		m_maxY = maxY;

		m_areaXSize = areaXSize;
		m_areaYSize = areaYSize;

		float sizeX = std::ceil(m_maxX - m_minX);
		float sizeY = std::ceil(m_maxY - m_minY);
		m_countX = static_cast<int32_t>(std::ceil(sizeX / m_areaXSize));
		m_countY = static_cast<int32_t>(std::ceil(sizeY / m_areaYSize));

		m_areas = static_cast<AreaType*>(std::malloc(sizeof(AreaType) * m_countX * m_countY));
		for (int32_t y = 0; y < m_countY; ++y)
		{
			for (int32_t x = 0; x < m_countX; ++x)
			{
				AreaType* area = get_area(x, y);
				if (area != nullptr)
				{
					new (area) AreaType(x, y, std::forward<Args>(args)...);
				}
			}
		}

		link(LINK_RADIUS);
	}

	template<class WorkerType>
	void work(WorkerType worker)
	{
		for (int32_t y = 0; y < m_countY; ++y)
		{
			for (int32_t x = 0; x < m_countX; ++x)
			{
				AreaType* area = get_area(x, y);
				if (area != nullptr)
				{
					worker(x, y, area);
				}
			}
		}
	}

	AreaType* get_area(int32_t x, int32_t y) const
	{
		if ((0 <= x && x < m_countX) &&
			(0 <= y && y < m_countY))
		{
			return &m_areas[(y * m_countX) + x];
		}

		return nullptr;
	}

	std::vector<AreaType*> select(int32_t x, int32_t y, int32_t radius) const
	{
		std::vector<AreaType*> areas;
		if (radius <= 0)
		{
			AreaType* area = get_area(x, y);
			if (area != nullptr)
			{
				areas.push_back(area);
			}

			return areas;
		}

		int32_t minX = (std::max)(x - radius, 0);
		int32_t minY = (std::max)(y - radius, 0);
		int32_t maxX = (std::min)(x + radius, m_countX - 1);
		int32_t maxY = (std::min)(y + radius, m_countY - 1);

		for (int32_t tempY = minY; tempY <= maxY; ++tempY)
		{
			for (int32_t tempX = minX; tempX <= maxX; ++tempX)
			{
				AreaType* area = get_area(tempX, tempY);
				if (area != nullptr)
				{
					areas.push_back(area);
				}
			}
		}

		return areas;
	}

	int32_t getX(float x) const { return static_cast<int32_t>((x - m_minX) / m_areaXSize); }
	int32_t getY(float y) const { return static_cast<int32_t>((y - m_minY) / m_areaYSize); }

	AreaType* get_area(float x, float y) const
	{
		return get_area(getX(x), getY(y));
	}

	std::vector<AreaType*> select(float x, float y, int32_t radius) const
	{
		return select(getX(x), getY(y), radius);
	}

private:

	void link(int32_t radius)
	{
		auto linker = [this, radius](int32_t x, int32_t y, AreaType* area)
			{
				auto areas = select(x, y, radius);
				for (auto temp : areas)
				{
					if (area != temp)
					{
						area->add_ref(temp);
					}
				}
			};

		work(linker);
	}

	void clear()
	{
		if (m_areas != nullptr)
		{
			for (int32_t y = 0; y < m_countY; ++y)
			{
				for (int32_t x = 0; x < m_countX; ++x)
				{
					AreaType* area = get_area(x, y);
					if (area != nullptr)
					{
						area->~AreaType();
					}
				}
			}

			std::free(m_areas);
			m_areas = nullptr;
		}

		m_countX = 0;
		m_countY = 0;

		m_minX = 0;
		m_minY = 0;
		m_maxX = 0;
		m_maxY = 0;

		m_areaXSize = 0;
		m_areaYSize = 0;
	}

};



