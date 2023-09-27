
#pragma once

#include <cstdint>
#include <chrono>


class Task
{
public:

	virtual ~Task()
	{
	}

	virtual void run() = 0;

};


static int64_t get_now_tick()
{
	static_assert(std::chrono::high_resolution_clock::is_steady == true);
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}
