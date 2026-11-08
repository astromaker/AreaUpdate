#include "RadiusExecuter.h"
#include <stdexcept>
#include <cstdio>

namespace
{
    int64_t radius_now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
} // namespace

RadiusLocks::RadiusLocks(size_t cells)
    : m_cells(cells)
{
    if (cells == 0)
        throw std::invalid_argument("Empty radius lock table");

    const auto count = cells / 64 + (cells % 64 != 0);
    m_words = std::make_unique<std::atomic_uint64_t[]>(count);
    for (size_t index = 0; index < count; ++index)
        m_words[index].store(0);
}

std::vector<RadiusMask> RadiusLocks::footprint(std::vector<size_t> cells) const
{
    std::sort(cells.begin(), cells.end());
    std::vector<RadiusMask> masks{};

    for (auto cell : cells)
    {
        if (cell >= m_cells)
            throw std::out_of_range("Radius cell outside lock table");

        auto word = &m_words[cell / 64];
        if (masks.empty() || masks.back().word != word)
            masks.push_back({word, 0});
        masks.back().bits |= uint64_t{1} << (cell % 64);
    }
    return masks;
}

void RadiusArea::set_footprint(std::vector<RadiusMask> masks)
{
    if (masks.empty())
        throw std::invalid_argument("Empty radius footprint");
    for (const auto& mask : masks)
    {
        if (mask.word == nullptr || mask.bits == 0)
            throw std::invalid_argument("Invalid radius mask");
    }

    m_masks = std::move(masks);
}

bool RadiusArea::try_acquire(RadiusSample* sample)
{
    if (m_masks.empty())
        throw std::logic_error("Configure radius footprint before execution");

    const auto begin = sample != nullptr ? radius_now_ns() : int64_t{0};
    if (sample != nullptr)
        ++sample->attempts;

    for (size_t index = 0; index < m_masks.size(); ++index)
    {
        const auto& mask = m_masks[index];
        auto observed = mask.word->load(std::memory_order_relaxed);
        bool acquired = false;
        while ((observed & mask.bits) == 0)
        {
            if (mask.word->compare_exchange_weak(observed, observed | mask.bits, std::memory_order_acquire,
                                                 std::memory_order_relaxed))
            {
                acquired = true;
                break;
            }
        }
        if (acquired == false)
        {
            const auto rollbackBegin = sample != nullptr ? radius_now_ns() : int64_t{0};
            for (size_t rollback = 0; rollback < index; ++rollback)
                m_masks[rollback].word->fetch_and(~m_masks[rollback].bits, std::memory_order_release);

            if (sample != nullptr)
            {
                sample->acquireNs += rollbackBegin - begin;
                sample->rollbackNs += radius_now_ns() - rollbackBegin;
            }

            return false;
        }
    }
    if (sample != nullptr)
        sample->acquireNs += radius_now_ns() - begin;
    return true;
}

void RadiusArea::release()
{
    for (const auto& mask : m_masks)
        mask.word->fetch_and(~mask.bits, std::memory_order_release);
}

void RadiusExecuter::configure_queue_trace(RadiusArea* area, bool enabled)
{
    std::lock_guard<std::mutex> guard(m_workersLock);
    if (area == nullptr || m_workers.empty() == false || m_ready.load() != 0 || m_runners.load() != 0)
        throw std::logic_error("Configure radius queue trace before publication and Worker attachment");

    if (enabled)
    {
        m_queueTraces.emplace(area, RadiusQueueTrace{});
    }
    else
    {
        m_queueTraces.erase(area);
    }
}

RadiusQueueTrace* RadiusExecuter::queue_trace(RadiusArea* area)
{
    if (m_queueTraces.empty())
        return nullptr;

    const auto found = m_queueTraces.find(area);
    return found == m_queueTraces.end() ? nullptr : &found->second;
}

void RadiusExecuter::trace_publication(RadiusArea* area, const Queue& queue)
{
    auto trace = queue_trace(area);
    if (trace == nullptr)
        return;

    *trace = {};
    trace->publishedNs = radius_now_ns();
    if (m_queueDiagnostics)
    {
        trace->holAtPublishNs = queue.hol_clock(trace->publishedNs);
        trace->holObserved = true;
    }
}

void RadiusArea::update(RadiusSample* sample)
{
    try
    {
        do_update();
    }
    catch (...)
    {
        release();
        throw;
    }
    const auto begin = sample != nullptr ? radius_now_ns() : int64_t{0};
    release();
    if (sample != nullptr)
    {
        sample->releaseNs += radius_now_ns() - begin;
        ++sample->updates;
    }
}

void RadiusExecuter::configure(size_t batchSize, size_t profileStride, std::function<void()> afterBatch)
{
    if (batchSize == 0 || batchSize > BATCH_SIZE)
        throw std::invalid_argument("Radius batch must be between 1 and 64");

    m_batchSize = batchSize;
    m_profileStride = profileStride;
    m_afterBatch = std::move(afterBatch);
}

void RadiusExecuter::print_profile() const
{
    if (m_queueDiagnostics)
    {
        for (size_t index = 0; index < m_queues.size(); ++index)
        {
            const auto& queue = *m_queues[index];
            std::printf("RADIUS_QUEUE bucket=%zu visits=%llu lock_misses=%llu head_failures=%llu popped=%llu "
                        "closed_hol_ns=%llu\n",
                        index, queue.visits, queue.lockMisses.load(), queue.headFailures, queue.popped, queue.holNs);
        }
    }
    std::printf("RADIUS_PROFILE stride=%zu batch=%zu passes=%llu batches=%llu wake_calls=%llu wake_targets=%llu "
                "wake_accepted=%llu wake_samples=%llu wake_ns=%llu attempts=%llu sampled_updates=%llu acquire_ns=%llu "
                "rollback_ns=%llu release_ns=%llu hold_before_update_ns=%llu\n",
                m_profileStride, m_batchSize, m_passes.load(), m_batches.load(), m_wakeCalls.load(),
                m_wakeTargets.load(), m_wakeAccepted.load(), m_wakeSamples.load(), m_wakeNs.load(), m_profile.attempts,
                m_profile.updates, m_profile.acquireNs, m_profile.rollbackNs, m_profile.releaseNs,
                m_profile.holdBeforeUpdateNs);
    std::printf("RADIUS_PUBLICATION calls=%llu queue_locks=%llu areas=%llu\n", m_pushCalls.load(), m_pushLocks.load(),
                m_pushAreas.load());
    std::printf("RADIUS_TUNING fast=%d merge=%d fast_batches=%llu deferred=%llu flushes=%llu empty_wakes=%llu\n",
                static_cast<int>(m_fastBucket), static_cast<int>(m_mergeNotifications), m_fastBatches.load(),
                m_deferredWakes.load(), m_notificationFlushes.load(), m_emptyWakes.load());
}

void RadiusExecuter::configure_publication(bool fastBucket, bool mergeNotifications)
{
    m_fastBucket = fastBucket;
    m_mergeNotifications = mergeNotifications;
}

void RadiusExecuter::configure_queue_diagnostics(bool enabled)
{
    std::lock_guard<std::mutex> guard(m_workersLock);
    if (m_workers.empty() == false || m_ready.load() != 0 || m_runners.load() != 0)
        throw std::logic_error("Configure queue diagnostics before publication and Worker attachment");

    m_queueDiagnostics = enabled;
}

void RadiusExecuter::notify_publication(bool emptyQueue)
{
    ++m_generation;
    auto& state = local_state();
    // A producer can run a long callback before its next ticker entry. Wake idle consumers on first publication.
    if (emptyQueue == false && m_mergeNotifications && state.owner == this && state.attached && currentWorker != nullptr
        && currentWorker->is_running())
    {
        state.pendingWake = true;
        if (m_profileStride != 0)
            ++m_deferredWakes;
        return;
    }

    if (emptyQueue && m_profileStride != 0)
        ++m_emptyWakes;

    wake_workers();
}

void RadiusExecuter::flush_notifications(LocalState& state)
{
    if (state.pendingWake == false)
        return;

    state.pendingWake = false;
    if (m_ready.load() != 0)
    {
        if (m_profileStride != 0)
            ++m_notificationFlushes;
        wake_workers();
    }
}

RadiusExecuter::RadiusExecuter(size_t queues)
{
    for (size_t index = 0; index < (std::max)(size_t{1}, queues); ++index)
        m_queues.push_back(std::make_unique<Queue>());
}

RadiusExecuter::LocalState& RadiusExecuter::local_state()
{
    static thread_local LocalState state{};
    return state;
}

void RadiusExecuter::wake_workers()
{
    const bool sampled = m_profileStride != 0 && m_wakeCalls.fetch_add(1) % m_profileStride == 0;
    const auto begin = sampled ? radius_now_ns() : int64_t{0};
    size_t targets = 0, accepted = 0;
    std::lock_guard<std::mutex> guard(m_workersLock);
    for (auto worker : m_workers)
    {
        if (worker != currentWorker)
        {
            const bool notified = worker->wake();
            if (m_profileStride != 0)
            {
                ++targets;
                accepted += notified;
            }
        }
    }
    if (m_profileStride != 0)
    {
        m_wakeTargets.fetch_add(targets, std::memory_order_relaxed);
        m_wakeAccepted.fetch_add(accepted, std::memory_order_relaxed);
    }
    if (sampled)
    {
        ++m_wakeSamples;
        m_wakeNs.fetch_add(radius_now_ns() - begin, std::memory_order_relaxed);
    }
}

void RadiusExecuter::push(RadiusArea* area, size_t bucket)
{
    if (m_profileStride != 0)
    {
        ++m_pushCalls;
        ++m_pushLocks;
        ++m_pushAreas;
    }
    auto& queue = *m_queues[bucket % m_queues.size()];
    ++m_ready;
    size_t depth = 0;
    {
        std::lock_guard<std::mutex> guard(queue.mutex);
        queue.areas.push_back(area);
        trace_publication(area, queue);
        depth = queue.areas.size();
    }
    if (depth == 1 || depth % BATCH_SIZE == 0 || m_runners.load() == 0)
    {
        notify_publication(depth == 1);
    }
}

void RadiusExecuter::push_batch(const RadiusSubmission* submissions, size_t count)
{
    if (count == 0)
        return;
    if (submissions == nullptr || count > BATCH_SIZE)
        throw std::invalid_argument("Invalid radius publication batch");

    std::array<size_t, BATCH_SIZE> order{};
    const auto firstBucket = submissions[0].bucket % m_queues.size();
    bool homogeneous = m_fastBucket;
    for (size_t index = 0; index < count; ++index)
    {
        if (submissions[index].area == nullptr)
            throw std::invalid_argument("Null radius submission");
        order[index] = index;
        if (homogeneous && submissions[index].bucket % m_queues.size() != firstBucket)
            homogeneous = false;
    }
    if (homogeneous == false)
    {
        std::sort(order.begin(), order.begin() + count,
                  [&](size_t left, size_t right)
                  {
                      const auto leftBucket = submissions[left].bucket % m_queues.size();
                      const auto rightBucket = submissions[right].bucket % m_queues.size();
                      return leftBucket != rightBucket ? leftBucket < rightBucket : left < right;
                  });
    }
    else if (m_profileStride != 0)
    {
        ++m_fastBatches;
    }

    // Reserve before the first publication so concurrent consumers cannot underflow ready work.
    m_ready.fetch_add(count);
    size_t published = 0, locks = 0;
    bool emptyQueue = false;
    try
    {
        while (published < count)
        {
            const auto bucket = homogeneous ? firstBucket : submissions[order[published]].bucket % m_queues.size();
            auto& queue = *m_queues[bucket];
            {
                std::lock_guard<std::mutex> guard(queue.mutex);
                emptyQueue = emptyQueue || queue.areas.empty();
                ++locks;
                do
                {
                    queue.areas.push_back(submissions[order[published]].area);
                    trace_publication(submissions[order[published]].area, queue);
                    ++published;
                } while (published < count
                         && (homogeneous || submissions[order[published]].bucket % m_queues.size() == bucket));
            }
#ifdef AREAUPDATE_TEST_HOOKS
            if (m_groupHook != nullptr)
                m_groupHook(m_groupContext);
#endif
        }
    }
    catch (...)
    {
        m_ready.fetch_sub(count - published);
        ++m_generation;
        wake_workers();
        throw;
    }

    if (m_profileStride != 0)
    {
        ++m_pushCalls;
        m_pushLocks.fetch_add(locks, std::memory_order_relaxed);
        m_pushAreas.fetch_add(count, std::memory_order_relaxed);
    }
#ifdef AREAUPDATE_TEST_HOOKS
    if (m_publishHook != nullptr)
        m_publishHook(m_publishContext);
#endif
    // External producers notify immediately; pool Workers may flush at their next ticker entry.
    notify_publication(emptyQueue);
}

bool RadiusExecuter::on_tick()
{
    auto& state = local_state();
    if (state.owner != this)
    {
        state = {};
        state.owner = this;
        state.next = currentWorker != nullptr ? currentWorker->get_id() % m_queues.size() : 0;
        if (currentWorker != nullptr)
        {
            std::lock_guard<std::mutex> guard(m_workersLock);
            state.attached = std::find(m_workers.begin(), m_workers.end(), currentWorker) != m_workers.end();
        }
    }
    flush_notifications(state);
    state.next %= m_queues.size();
    state.generation = m_generation.load();
    state.progressed = false;

    std::array<RadiusArea*, BATCH_SIZE> areas{};
    std::array<int64_t, BATCH_SIZE> acquiredAt{};
    RadiusSample sample{};
    const bool sampled = m_profileStride != 0 && m_passes.fetch_add(1) % m_profileStride == 0;
    auto profile = sampled ? &sample : nullptr;
    uint64_t failures = 0;
    for (size_t visited = 0; visited < m_queues.size(); ++visited)
    {
        auto& queue = *m_queues[state.next];
        state.next = (state.next + 1) % m_queues.size();
        size_t count = 0;
        {
            std::unique_lock<std::mutex> guard(queue.mutex, std::try_to_lock);
            if (guard.owns_lock() == false)
            {
                if (m_queueDiagnostics)
                    queue.lockMisses.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (m_queueDiagnostics)
                ++queue.visits;

            while (queue.areas.empty() == false && count < m_batchSize)
            {
                auto area = queue.areas.front();
                auto trace = queue_trace(area);
                const auto probeBegin = trace != nullptr ? radius_now_ns() : int64_t{0};
                if (trace != nullptr && trace->firstProbeNs == 0)
                {
                    trace->firstProbeNs = probeBegin;
                    if (m_queueDiagnostics)
                    {
                        const auto elapsed = static_cast<uint64_t>(probeBegin - trace->publishedNs);
                        trace->holBeforeProbeNs =
                            (std::min)(elapsed, queue.hol_clock(probeBegin) - trace->holAtPublishNs);
                    }
                }

                const bool acquired = area->try_acquire(profile);
                if (trace != nullptr)
                {
                    const auto probeEnd = radius_now_ns();
                    trace->attemptNs += probeEnd - probeBegin;
                    if (acquired)
                    {
                        trace->acquiredNs = probeEnd;
                    }
                    else
                    {
                        ++trace->failures;
                    }
                }
                if (acquired == false)
                {
                    if (m_queueDiagnostics)
                    {
                        ++queue.headFailures;
                        if (queue.holSinceNs == 0)
                            queue.holSinceNs = radius_now_ns();
                    }
                    ++failures;
                    break;
                }
                if (m_queueDiagnostics)
                {
                    if (queue.holSinceNs != 0)
                    {
                        queue.holNs += radius_now_ns() - queue.holSinceNs;
                        queue.holSinceNs = 0;
                    }
                    ++queue.popped;
                }
                queue.areas.pop_front();
                acquiredAt[count] = sampled ? radius_now_ns() : int64_t{0};
                areas[count++] = area;
            }
        }
        if (count == 0)
            continue;

        ++m_runners;
        m_ready.fetch_sub(count);
        for (size_t index = 0; index < count; ++index)
        {
            if (sampled)
                sample.holdBeforeUpdateNs += radius_now_ns() - acquiredAt[index];
            areas[index]->update(profile);
        }
        if (m_afterBatch)
            m_afterBatch();
        if (m_profileStride != 0)
            ++m_batches;
        m_updates.fetch_add(count, std::memory_order_relaxed);
        --m_runners;

        state.progressed = true;
        if (failures != 0)
            m_failures.fetch_add(failures, std::memory_order_relaxed);
        ++m_generation;
        state.pendingWake = false;
        if (m_ready.load() != 0)
            wake_workers();
        if (sampled)
        {
            std::lock_guard<std::mutex> guard(m_profileLock);
            m_profile.acquireNs += sample.acquireNs;
            m_profile.rollbackNs += sample.rollbackNs;
            m_profile.releaseNs += sample.releaseNs;
            m_profile.holdBeforeUpdateNs += sample.holdBeforeUpdateNs;
            m_profile.attempts += sample.attempts;
            m_profile.updates += sample.updates;
        }
        return false;
    }
    if (failures != 0)
        m_failures.fetch_add(failures, std::memory_order_relaxed);
    if (sampled)
    {
        std::lock_guard<std::mutex> guard(m_profileLock);
        m_profile.acquireNs += sample.acquireNs;
        m_profile.rollbackNs += sample.rollbackNs;
        m_profile.attempts += sample.attempts;
    }
    return true;
}

bool RadiusExecuter::has_work() const
{
    const auto& state = local_state();
    return m_ready.load() != 0 && (state.owner != this || state.progressed || state.generation != m_generation.load());
}

TickerIdleState RadiusExecuter::idle_state() const
{
    return {m_ready.load(), m_runners.load(), has_work()};
}

void RadiusExecuter::attach(Worker* worker)
{
    std::lock_guard<std::mutex> guard(m_workersLock);
    m_workers.push_back(worker);
}

void RadiusExecuter::detach(Worker* worker)
{
    std::lock_guard<std::mutex> guard(m_workersLock);
    m_workers.erase(std::remove(m_workers.begin(), m_workers.end(), worker), m_workers.end());
}
