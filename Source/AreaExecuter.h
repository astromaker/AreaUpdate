
#pragma once

#include <cstdint>

#include <list>
#include <vector>
#include <shared_mutex>

#include "Area.h"
#include "Worker.h"


class AreaQueue
{
	mutable std::shared_mutex m_rwlock;
	std::list<Area*> m_areas;

public:

	AreaQueue() = default;
	~AreaQueue() = default;

	bool empty() const;

	Area* pop();
	std::vector<Area*> pop_all(size_t count = 0);
	std::vector<Area*> try_pop(size_t count = 0);

	void push(Area* area);

private:

	std::vector<Area*> pop_n(size_t count);

};

class AreaExecuter : public Ticker
{
	std::vector<AreaQueue*> m_queues;

public:

	explicit AreaExecuter(size_t count);
	~AreaExecuter();

	void push(Area* area);

	bool run();

	bool on_tick() override;

private:

	void setup(size_t count);

	size_t init_index() const;

};

