
#pragma once

#include <cstdint>
#include <thread>
#include <atomic>
#include <shared_mutex>
#include <vector>
#include <chrono>

#include "Task.h"
#include "PriorityTimer.h"


class Ticker
{
public:

	virtual ~Ticker() = default;

	// 모든 작업을 마치면 true, 남은 작업이 있다면 false
	virtual bool on_tick() = 0;

};

class Worker
{
	std::thread m_thread;
	std::atomic_bool m_running{ false };

	std::shared_mutex m_rwlock;
	std::vector<Task*> m_tasks;

	PriorityTimer m_timer;

	Ticker* m_ticker = nullptr;

public:

	Worker() = default;
	~Worker() = default;

	void start(Ticker* ticker);
	void stop();

	void join();

	void add_task(Task* task);
	void add_task(Task* task, int64_t timeout);

private:

	void setup();
	void cleanup();

	void run();

	std::chrono::milliseconds get_idle_tick();
	bool is_busy();

	void run_timer(int64_t now_tick);
	void run_task();
	bool run_ticker();

};

extern thread_local Worker* LWorker;

