#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

// A retained, coalesced signal closes the gap between checking work and sleeping.
class WakeSignal
{
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_signaled = false;
    std::chrono::steady_clock::time_point m_signalTime{};

public:
    void signal(std::chrono::steady_clock::time_point requested = {})
    {
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (m_signaled == false)
                m_signalTime = requested;
            m_signaled = true;
        }

        m_condition.notify_one();
    }

    void wait(std::chrono::steady_clock::time_point* signalTime = nullptr)
    {
        std::unique_lock<std::mutex> guard(m_mutex);
        m_condition.wait(guard,
                         [this]
                         {
                             return m_signaled;
                         });
        if (signalTime != nullptr)
            *signalTime = m_signalTime;
        m_signaled = false;
    }

    bool wait_until(std::chrono::steady_clock::time_point deadline,
                    std::chrono::steady_clock::time_point* signalTime = nullptr)
    {
        std::unique_lock<std::mutex> guard(m_mutex);
        const bool signaled = m_condition.wait_until(guard, deadline,
                                                     [this]
                                                     {
                                                         return m_signaled;
                                                     });
        if (signalTime != nullptr)
            *signalTime = signaled ? m_signalTime : std::chrono::steady_clock::time_point{};
        m_signaled = false;
        return signaled;
    }
};
