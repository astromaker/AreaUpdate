
#include "Area.h"

#include <cassert>
#include <iterator>

std::atomic_uint64_t FAIL_COUNT{ 0 }; // µð¹ö±ë¿ë
std::atomic_uint64_t RUN_COUNT{ 0 }; // µð¹ö±ë¿ë


void Area::add_ref(Area* area)
{
	m_refs.push_back(area);
}

bool Area::can()
{
	if (try_lock() == false)
	{
		return false;
	}

	auto pos = m_refs.begin();
	auto end = m_refs.end();
	for (; pos != end; ++pos)
	{
		Area* area = (*pos);
		if (area->try_lock() == false)
		{
			++FAIL_COUNT;

			auto rpos = std::make_reverse_iterator(pos);
			auto rend = m_refs.rend();
			for (; rpos != rend; ++rpos)
			{
				Area* temp = (*rpos);
				temp->unlock();
			}

			unlock();
			return false;
		}
	}

	return true;
}

void Area::update()
{
	do_update();

	release();

	++RUN_COUNT;
}

bool Area::try_lock()
{
	bool expected = false;
	if (m_lock.compare_exchange_strong(expected, true) == false)
	{
		assert(expected == true);
		return false;
	}

	return true;
}

void Area::unlock()
{
	assert(m_lock == true);
	m_lock = false;
}

void Area::release()
{
	for (auto area : m_refs)
	{
		area->unlock();
	}

	unlock();
}
