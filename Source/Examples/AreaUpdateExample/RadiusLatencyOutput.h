#pragma once

#include "RuntimeBackend.h"
#include <memory>
#include <mutex>

class RadiusLatencyProfile
{
    struct Cell
    {
        std::atomic_int64_t queuedNs{0};
        std::atomic_bool timer{false};
    };
    struct Sample
    {
        int32_t x = 0, y = 0;
        int64_t deadline = 0, queued = 0, begin = 0;
        RadiusQueueTrace phases{};
    };
    size_t m_side = 0, m_count = 0;
    std::unique_ptr<Cell[]> m_cells{};
    std::atomic_uint64_t m_samples{0}, m_timerNs{0}, m_queueNs{0}, m_maxTimerNs{0}, m_maxQueueNs{0};
    std::atomic_uint64_t m_phaseSamples{0}, m_publishNs{0}, m_dispatchNs{0}, m_retryNs{0}, m_batchNs{0};
    std::atomic_uint64_t m_attemptNs{0}, m_conflicted{0}, m_failures{0};
    std::atomic_uint64_t m_holBeforeProbeNs{0}, m_holSamples{0}, m_holDispatchNs{0};
    std::atomic_int64_t m_captureMinimum{50000000};
    std::mutex m_mutex;
    std::vector<Sample> m_slowest{};

    static void observe_max(std::atomic_uint64_t& target, uint64_t value)
    {
        auto current = target.load(std::memory_order_relaxed);
        while (current < value && target.compare_exchange_weak(current, value, std::memory_order_relaxed) == false)
        {
        }
    }

public:
    RadiusLatencyProfile(size_t side, size_t rows)
        : m_side(side)
        , m_count(side * rows)
        , m_cells(std::make_unique<Cell[]>(m_count))
    {
        m_slowest.reserve(32);
    }
    void observe(int32_t x, int32_t y, const RadiusRuntime::TraceEvent& event)
    {
        if (x < 0 || y < 0 || static_cast<size_t>(x) >= m_side)
            return;
        const auto index = static_cast<size_t>(y) * m_side + static_cast<size_t>(x);
        if (index >= m_count)
            return;

        auto& cell = m_cells[index];
        if (event.stage == RadiusRuntime::TraceStage::Queue)
        {
            cell.queuedNs.store(event.timeNs, std::memory_order_relaxed);
            cell.timer.store(event.timer, std::memory_order_relaxed);
            return;
        }
        if (event.stage != RadiusRuntime::TraceStage::Begin)
            return;
        const auto queued = cell.queuedNs.load(std::memory_order_relaxed);
        if (queued == 0)
            return;

        const auto deadline = event.deadlineTick * 1000000;
        const auto timerNs = cell.timer.load(std::memory_order_relaxed) ? (std::max)(int64_t{0}, queued - deadline) : 0;
        const auto queueNs = (std::max)(int64_t{0}, event.timeNs - queued);
        ++m_samples;
        m_timerNs.fetch_add(timerNs, std::memory_order_relaxed);
        m_queueNs.fetch_add(queueNs, std::memory_order_relaxed);
        observe_max(m_maxTimerNs, timerNs);
        observe_max(m_maxQueueNs, queueNs);
        const auto& phases = event.queueTrace;
        if (phases.publishedNs >= queued && phases.firstProbeNs >= phases.publishedNs
            && phases.acquiredNs >= phases.firstProbeNs && event.timeNs >= phases.acquiredNs)
        {
            ++m_phaseSamples;
            m_publishNs.fetch_add(phases.publishedNs - queued, std::memory_order_relaxed);
            m_dispatchNs.fetch_add(phases.firstProbeNs - phases.publishedNs, std::memory_order_relaxed);
            if (phases.holObserved)
            {
                ++m_holSamples;
                m_holBeforeProbeNs.fetch_add(phases.holBeforeProbeNs, std::memory_order_relaxed);
                m_holDispatchNs.fetch_add(phases.firstProbeNs - phases.publishedNs, std::memory_order_relaxed);
            }
            m_retryNs.fetch_add(phases.acquiredNs - phases.firstProbeNs, std::memory_order_relaxed);
            m_batchNs.fetch_add(event.timeNs - phases.acquiredNs, std::memory_order_relaxed);
            m_attemptNs.fetch_add(phases.attemptNs, std::memory_order_relaxed);
            m_conflicted.fetch_add(phases.failures != 0 ? 1 : 0, std::memory_order_relaxed);
            m_failures.fetch_add(phases.failures, std::memory_order_relaxed);
        }
        const auto late = event.timeNs - deadline;
        if (late < m_captureMinimum.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> guard(m_mutex);
        const Sample sample{x, y, deadline, queued, event.timeNs, phases};
        if (m_slowest.size() < 32)
        {
            m_slowest.push_back(sample);
        }
        else
        {
            auto worst = std::min_element(m_slowest.begin(), m_slowest.end(),
                                          [](const Sample& left, const Sample& right)
                                          {
                                              return left.begin - left.deadline < right.begin - right.deadline;
                                          });
            if (late > worst->begin - worst->deadline)
                *worst = sample;
        }
        if (m_slowest.size() == 32)
        {
            const auto worst = std::min_element(m_slowest.begin(), m_slowest.end(),
                                                [](const Sample& left, const Sample& right)
                                                {
                                                    return left.begin - left.deadline < right.begin - right.deadline;
                                                });
            m_captureMinimum.store(worst->begin - worst->deadline, std::memory_order_relaxed);
        }
    }
    void print_report()
    {
        const auto count = m_samples.load();
        std::printf("RADIUS_LATENCY samples=%llu mean_timer_ms=%.3f mean_area_wait_ms=%.3f max_timer_ms=%.3f "
                    "max_area_wait_ms=%.3f retained=%zu\n",
                    count, count != 0 ? m_timerNs.load() / 1000000.0 / count : 0.0,
                    count != 0 ? m_queueNs.load() / 1000000.0 / count : 0.0, m_maxTimerNs.load() / 1000000.0,
                    m_maxQueueNs.load() / 1000000.0, m_slowest.size());
        const auto phaseCount = m_phaseSamples.load();
        const auto scale = phaseCount != 0 ? 1000000.0 * phaseCount : 1.0;
        const auto holCount = m_holSamples.load();
        const auto holScale = holCount != 0 ? 1000000.0 * holCount : 1.0;
        std::printf("RADIUS_PHASE samples=%llu publish_ms=%.6f dispatch_ms=%.6f retry_ms=%.6f batch_ms=%.6f "
                    "attempt_ms=%.6f conflicted=%llu failures=%llu hol_samples=%llu hol_before_probe_ms=%.6f "
                    "non_hol_ms=%.6f\n",
                    phaseCount, m_publishNs.load() / scale, m_dispatchNs.load() / scale, m_retryNs.load() / scale,
                    m_batchNs.load() / scale, m_attemptNs.load() / scale, m_conflicted.load(), m_failures.load(),
                    holCount, holCount != 0 ? m_holBeforeProbeNs.load() / holScale : -1.0,
                    holCount != 0 ? (m_holDispatchNs.load() - m_holBeforeProbeNs.load()) / holScale : -1.0);
        std::sort(m_slowest.begin(), m_slowest.end(),
                  [](const Sample& left, const Sample& right)
                  {
                      return left.begin - left.deadline > right.begin - right.deadline;
                  });
        for (const auto& sample : m_slowest)
        {
            std::printf("RADIUS_LATE x=%d y=%d deadline_ns=%lld queued_ns=%lld begin_ns=%lld timer_ms=%.3f "
                        "area_wait_ms=%.3f total_ms=%.3f\n",
                        sample.x, sample.y, sample.deadline, sample.queued, sample.begin,
                        (sample.queued - sample.deadline) / 1000000.0, (sample.begin - sample.queued) / 1000000.0,
                        (sample.begin - sample.deadline) / 1000000.0);
            std::printf("RADIUS_PHASE_LATE x=%d y=%d publish_ms=%.6f dispatch_ms=%.6f retry_ms=%.6f batch_ms=%.6f "
                        "attempt_ms=%.6f failures=%llu\n",
                        sample.x, sample.y, (sample.phases.publishedNs - sample.queued) / 1000000.0,
                        (sample.phases.firstProbeNs - sample.phases.publishedNs) / 1000000.0,
                        (sample.phases.acquiredNs - sample.phases.firstProbeNs) / 1000000.0,
                        (sample.begin - sample.phases.acquiredNs) / 1000000.0, sample.phases.attemptNs / 1000000.0,
                        sample.phases.failures);
        }
    }
};
