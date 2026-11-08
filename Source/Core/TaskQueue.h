#pragma once

#include "Task.h"

// Multi-producer publication, single-consumer detach/reverse, as in PWQueue.
class TaskQueue
{
    std::atomic<Task*> m_head{nullptr};

public:
    static Task* next(Task* task)
    {
        return task->m_next;
    }

    void push(Task* task)
    {
        auto head = m_head.load(std::memory_order_relaxed);

        do
        {
            task->m_next = head;
        } while (m_head.compare_exchange_weak(head, task, std::memory_order_release, std::memory_order_relaxed)
                 == false);
    }

    Task* pop_all()
    {
        auto task = m_head.exchange(nullptr, std::memory_order_acquire);
        Task* result = nullptr;

        while (task != nullptr)
        {
            auto next = task->m_next;
            task->m_next = result;
            result = task;
            task = next;
        }

        return result;
    }

    bool empty() const
    {
        return m_head.load(std::memory_order_acquire) == nullptr;
    }
};
