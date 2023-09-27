
#include "AreaExecuter.h"


bool AreaQueue::empty() const
{
	std::shared_lock<std::shared_mutex> guard(m_rwlock);
	return m_areas.empty();
}

Area* AreaQueue::pop()
{
	std::unique_lock<std::shared_mutex> guard(m_rwlock);

	auto pos = m_areas.begin();
	auto end = m_areas.end();
	for (; pos != end; ++pos)
	{
		Area* area = (*pos);
		if (area->can() == true)
		{
			m_areas.erase(pos);
			return area;
		}
	}

	return nullptr;
}

std::vector<Area*> AreaQueue::pop_all(size_t count)
{
	std::unique_lock<std::shared_mutex> guard(m_rwlock);
	return pop_n(count);
}

std::vector<Area*> AreaQueue::try_pop(size_t count)
{
	std::unique_lock<std::shared_mutex> guard(m_rwlock, std::defer_lock_t());
	if (guard.try_lock() == false)
	{
		return {};
	}

	return pop_n(count);
}

void AreaQueue::push(Area* area)
{
	std::unique_lock<std::shared_mutex> guard(m_rwlock);
	m_areas.emplace_back(area);
}

std::vector<Area*> AreaQueue::pop_n(size_t count)
{
	std::vector<Area*> areas;

	auto pos = m_areas.begin();
	while (pos != m_areas.end())
	{
		if (count != 0)
		{
			if (areas.size() >= count)
			{
				break;
			}
		}

		Area* area = (*pos);
		if (area->can() == true)
		{
			pos = m_areas.erase(pos);
			areas.emplace_back(area);
		}
		else
		{
			break;
		}
	}

	return areas;
}



AreaExecuter::AreaExecuter(size_t count)
{
	setup(count);
}

AreaExecuter::~AreaExecuter()
{
	for (auto queue : m_queues)
	{
		delete queue;
	}

	m_queues.clear();
}

void AreaExecuter::push(Area* area)
{
	size_t idx = ::rand() % m_queues.size();
	m_queues[idx]->push(area);
}

bool AreaExecuter::run()
{
	thread_local size_t INDEX = init_index();

	size_t start = INDEX;
	while (true)
	{
		auto areas = m_queues[INDEX++]->try_pop();

		if (INDEX >= m_queues.size())
		{
			INDEX = 0;
		}

		if (areas.empty() == false)
		{
			for (auto area : areas)
			{
				area->update();
			}

			return false;
		}

		if (INDEX == start)
		{
			break;
		}
	}

	return (INDEX == start);
}

bool AreaExecuter::on_tick()
{
	return run();
}


void AreaExecuter::setup(size_t count)
{
	if (count == 0)
	{
		count = 1;
	}

	for (size_t i = 0; i < count; ++i)
	{
		m_queues.emplace_back(new AreaQueue());
	}
}

size_t AreaExecuter::init_index() const
{
	if (m_queues.empty() == true)
	{
		return 0;
	}

	return ::rand() % m_queues.size();
}
