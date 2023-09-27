
#pragma once

#include <vector>
#include <atomic>


extern std::atomic_uint64_t FAIL_COUNT; // µð¹ö±ë¿ë
extern std::atomic_uint64_t RUN_COUNT; // µð¹ö±ë¿ë


class Area
{
	std::vector<Area*> m_refs;
	std::atomic_bool m_lock{ false };

public:

	Area() = default;
	virtual ~Area() = default;

	void add_ref(Area* area);

	bool can();

	void update();

private:

	bool try_lock();
	void unlock();

	void release();

	virtual void do_update() = 0;

};
