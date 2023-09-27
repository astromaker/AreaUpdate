
#include <cstdio>
#include <atomic>
#include <limits>
#include <random>

#include "Worker.h"
#include "AreaExecuter.h"
#include "AreaManager.h"


constexpr size_t THREAD_COUNT = 8;
constexpr int64_t FPS = 20;
constexpr int64_t TICK = FPS > 0 ? 1000 / FPS : 0;

std::atomic_int32_t MAX_TICK_COUNT{ 0 };
std::atomic_int32_t MIN_TICK_COUNT{ (std::numeric_limits<int32_t>::max)() };

AreaExecuter Exceuter(THREAD_COUNT);
Worker Workers[THREAD_COUNT];


class AreaTicker : public Task
{
	Area* m_area = nullptr;

public:

	explicit AreaTicker(Area* area)
		: m_area(area)
	{
	}

	void run() override
	{
		Exceuter.push(m_area);
	}

};


class MyArea : public Area
{
	int32_t m_x = 0; // 디버깅용
	int32_t m_y = 0; // 디버깅용

	int32_t m_updateCount = 0;
	int64_t m_updateTick = 0;

public:

	MyArea(int32_t x, int32_t y)
		: m_x(x)
		, m_y(y)
		, m_updateTick(get_now_tick())
	{
	}

	int32_t getX() const { return m_x; }
	int32_t getY() const { return m_y; }

private:

	void do_update() override
	{
		++m_updateCount;

		auto nowTick = get_now_tick();
		auto runTick = nowTick - m_updateTick;
		if (runTick >= 1000)
		{
			//printf("[%d]: %d\n", idx_, updateCount_);

			if (MAX_TICK_COUNT < m_updateCount)
			{
				MAX_TICK_COUNT = m_updateCount;
			}
			if (MIN_TICK_COUNT > m_updateCount)
			{
				MIN_TICK_COUNT = m_updateCount;
			}

			m_updateCount = 0;
			m_updateTick = nowTick;
		}

		auto ticker = new AreaTicker(this);
		LWorker->add_task(ticker, TICK);
	}
};



int main()
{
	for (auto& worker : Workers)
	{
		worker.start(&Exceuter);
	}

#ifdef _DEBUG
	float minX = 0;
	float minY = 0;
	float maxX = 400;
	float maxY = 400;
#else
	float minX = 0;
	float minY = 0;
	float maxX = 16 * 1000;
	float maxY = 16 * 1000;
#endif
	int32_t areaX = 50;
	int32_t areaY = 50;

	AreaManager<MyArea> manager;
	manager.build(minX, minY, maxX, maxY, areaX, areaY);

	size_t areaCount = 0;
	std::mt19937 engine{ std::random_device{}() };
	auto starter = [&areaCount, &engine](int32_t, int32_t, MyArea* area)
		{
			++areaCount;

			std::uniform_int_distribution<int64_t> dist(0, TICK);
			size_t idx = engine() % THREAD_COUNT;

			auto ticker = new AreaTicker(area);
			Workers[idx].add_task(ticker, dist(engine)); // 초기 tick 분산
		};
	manager.work(starter);

	while (true)
	{
		auto runCount = RUN_COUNT.load();
		auto minTick = MIN_TICK_COUNT.load();
		printf("areaCount: %llu, FPS: %lld, tick: (average: %llu, min: %d, max: %d), runCount: %llu, failCount: %llu\n",
			areaCount, FPS,
			runCount / areaCount,
			minTick == (std::numeric_limits<int32_t>::max)() ? 0 : minTick,
			MAX_TICK_COUNT.load(),
			runCount, FAIL_COUNT.load());

		RUN_COUNT = 0;
		FAIL_COUNT = 0;
		MAX_TICK_COUNT = 0;
		MIN_TICK_COUNT = (std::numeric_limits<int32_t>::max)();

		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	for (auto& worker : Workers)
	{
		worker.stop();
	}

	for (auto& worker : Workers)
	{
		worker.join();
	}

	return 0;
}
