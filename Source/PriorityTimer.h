
#pragma once

#include <cstdint>
#include <vector>
#include <queue>
#include <shared_mutex>

#include "Task.h"


class PriorityTimer
{
	struct Node
	{
		Task* task = nullptr;
		int64_t expire = 0;

		Node(Task* t, int64_t e)
			: task(t)
			, expire(e)
		{
		}
	};

	struct CompareNode
	{
		bool operator()(const Node& left, const Node& right) const
		{
			return (left.expire > right.expire);
		}
	};

	mutable std::shared_mutex m_rwlock;
	std::priority_queue<Node, std::vector<Node>, CompareNode> m_queue;

public:

	PriorityTimer() = default;
	~PriorityTimer() = default;

	void push(Task* t, int64_t timeout)
	{
		int64_t expire = get_now_tick() + timeout;

		std::unique_lock<std::shared_mutex> guard(m_rwlock);
		m_queue.emplace(t, expire);
	}

	bool has_expired_task() const
	{
		std::shared_lock<std::shared_mutex> guard(m_rwlock);

		auto now_tick = get_now_tick();
		if (m_queue.empty() == false)
		{
			auto& n = m_queue.top();
			if (n.expire <= now_tick)
			{
				return true;
			}
		}

		return false;
	}

	std::vector<Task*> expired_tasks(int64_t now_tick)
	{
		std::vector<Task*> tasks;
		{
			std::unique_lock<std::shared_mutex> guard(m_rwlock);
			while (m_queue.empty() == false)
			{
				auto& node = m_queue.top();
				if (node.expire > now_tick)
				{
					break;
				}

				tasks.emplace_back(node.task);
				m_queue.pop();
			}
		}

		return tasks;
	}

};

