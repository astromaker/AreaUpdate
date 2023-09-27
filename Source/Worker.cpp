
#include "Worker.h"

#include <cstdlib>
#include <random>

thread_local Worker* LWorker = nullptr;


void Worker::start(Ticker* ticker)
{
	m_running = true;
	m_ticker = ticker;

	m_thread = std::thread(&Worker::run, this);
}

void Worker::stop()
{
	auto old = m_running.exchange(false);
	if (old == false)
	{
		return;
	}
}

void Worker::join()
{
	m_thread.join();
}

void Worker::add_task(Task* task)
{
	std::unique_lock<std::shared_mutex> guard(m_rwlock);
	m_tasks.push_back(task);
}

void Worker::add_task(Task* task, int64_t timeout)
{
	m_timer.push(task, timeout);
}

void Worker::setup()
{
	std::srand(std::random_device{}());

	LWorker = this;
}

void Worker::cleanup()
{
	LWorker = nullptr;
}

void Worker::run()
{
	setup();

	while (m_running == true)
	{
		auto now_tick = get_now_tick();

		run_timer(now_tick);
		run_task();

		if (run_ticker() == true)
		{
			std::chrono::milliseconds tick = get_idle_tick();
			if (tick > std::chrono::milliseconds(0))
			{
				std::this_thread::sleep_for(tick);
			}
		}
	}

	cleanup();
}

std::chrono::milliseconds Worker::get_idle_tick()
{
	if (is_busy() == true)
	{
		return std::chrono::milliseconds(0);
	}

	return std::chrono::milliseconds(1);
}

bool Worker::is_busy()
{
	{
		std::shared_lock<std::shared_mutex> guard(m_rwlock);
		if (m_tasks.empty() == false)
		{
			return true;
		}
	}

	if (m_timer.has_expired_task() == true)
	{
		return true;
	}

	return false;
}

void Worker::run_timer(int64_t now_tick)
{
	std::vector<Task*> tasks = m_timer.expired_tasks(now_tick);
	for (auto task : tasks)
	{
		task->run();

		delete task;
	}
}

void Worker::run_task()
{
	std::vector<Task*> tasks;
	{
		std::unique_lock<std::shared_mutex> guard(m_rwlock);
		m_tasks.swap(tasks);
	}

	for (auto task : tasks)
	{
		task->run();

		delete task;
	}
}

bool Worker::run_ticker()
{
	if (m_ticker == nullptr)
	{
		return true;
	}

	return m_ticker->on_tick();
}
