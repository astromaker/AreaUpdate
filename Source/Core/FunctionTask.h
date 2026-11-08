#pragma once

#include "Task.h"
#include <functional>
#include <utility>

class FunctionTask final : public Task
{
    std::function<void()> m_function;

public:
    explicit FunctionTask(std::function<void()> function)
        : m_function(std::move(function))
    {
    }

    void run() override
    {
        m_function();
    }
};
