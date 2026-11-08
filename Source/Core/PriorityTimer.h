
#pragma once

#include <cstdint>
#include <vector>
#include <queue>
#include <limits>

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

    class Queue : public std::priority_queue<Node, std::vector<Node>, CompareNode>
    {
    public:
        size_t retained_bytes() const
        {
            return this->c.capacity() * sizeof(Node);
        }
    };
    Queue m_queue;

public:
    PriorityTimer() = default;
    PriorityTimer(const PriorityTimer&) = delete;
    PriorityTimer& operator=(const PriorityTimer&) = delete;
    ~PriorityTimer()
    {
        clear();
    }

    // Single Worker owns this heap; external threads publish requests instead.
    void push_at(Task* task, int64_t expire)
    {
        m_queue.emplace(task, expire);
    }
    size_t retained_bytes() const
    {
        return m_queue.retained_bytes();
    }
    int64_t next_expire_tick() const
    {
        return m_queue.empty() ? (std::numeric_limits<int64_t>::max)() : m_queue.top().expire;
    }
    void clear()
    {
        while (m_queue.empty() == false)
        {
            delete m_queue.top().task;
            m_queue.pop();
        }
    }

    std::vector<Task*> expired_tasks(int64_t nowTick, size_t limit = 256)
    {
        std::vector<Task*> tasks;

        {
            while (m_queue.empty() == false && tasks.size() < limit)
            {
                auto& node = m_queue.top();
                if (node.expire > nowTick)
                    break;

                tasks.emplace_back(node.task);
                m_queue.pop();
            }
        }

        return tasks;
    }
};
