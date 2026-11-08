
#include "AreaExecuter.h"
#include "UpdateTickPlan.h"
#include <algorithm>
#include <unordered_map>
#include <stdexcept>
#include <limits>

namespace
{
    int64_t profile_now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
} // namespace

void AreaProfileDistribution::observe(uint64_t value)
{
    ++count;
    sum += value;
    maximum = (std::max)(maximum, value);
    size_t bucket = 0;
    uint64_t upper = 1;
    while (value > upper && bucket + 1 < histogram.size())
    {
        upper <<= 1;
        ++bucket;
    }
    ++histogram[bucket];
}

void AreaProfileDistribution::merge(const AreaProfileDistribution& other)
{
    count += other.count;
    sum += other.sum;
    maximum = (std::max)(maximum, other.maximum);
    for (size_t i = 0; i < histogram.size(); ++i)
        histogram[i] += other.histogram[i];
}

uint64_t AreaProfileDistribution::percentile_upper(unsigned percent) const
{
    if (count == 0)
        return 0;

    const auto target = (count * percent + 99) / 100;
    uint64_t accumulated = 0;
    for (size_t i = 0; i < histogram.size(); ++i)
    {
        accumulated += histogram[i];
        if (accumulated >= target)
            return i + 1 == histogram.size() ? maximum : (std::min)(maximum, uint64_t{1} << i);
    }
    return maximum;
}

struct AreaProfileState
{
    struct Queued
    {
        int64_t callbackNs = 0;
        int64_t registeredNs = 0;
        int64_t extractedNs = 0;
        uint64_t profileLockNs = 0;
        int64_t deadlineTick = 0;
        bool timerCallback = false;
    };
    size_t stride = 0;
    int64_t startNs = 0;
    int64_t firstCompletionNs = 0;
    int64_t epochStartNs = 0;
    size_t epochAreas = 0;
    uint64_t epochSequence = 0;
    std::vector<AreaProfileSecond> seconds{};
    std::unordered_map<const Area*, Queued> queued{};
    std::unordered_map<const Worker*, int64_t> blocked{};
    std::vector<AreaProfileEpoch> epochs{};
    std::vector<AreaProfilePartition> partitions{};

    bool selected(const Area* area) const
    {
        const auto tag = static_cast<uint32_t>(area->get_x()) * uint64_t{73856093}
                         ^ static_cast<uint32_t>(area->get_y()) * uint64_t{19349663};
        return tag % stride == 0;
    }
    AreaProfileSecond& row(int64_t nowNs)
    {
        const auto index = static_cast<size_t>((std::max)(int64_t{0}, nowNs - startNs) / 1000000000);
        return seconds[(std::min)(index, seconds.size() - 1)];
    }

    void finish_epoch(size_t partition, size_t admissions, size_t remaining, bool exhausted, int64_t now)
    {
        const auto duration = static_cast<uint64_t>(now - epochStartNs);
        auto& second = row(now);
        second.epochAreas.observe(epochAreas);
        second.epochAdmissions.observe(admissions);
        second.epochRemainingAreas.observe(remaining);
        second.epochNs.observe(duration);
        auto& total = partitions[partition];
        ++total.epochs;
        total.admissions += admissions;
        total.areas += epochAreas;
        total.remainingAreasSum += remaining;
        total.maxRemainingAreas = (std::max)(total.maxRemainingAreas, remaining);
        total.durationNs += duration;
        if (exhausted)
        {
            ++second.budgetExhaustedEpochs;
            ++total.budgetExhaustedEpochs;
            if (remaining != 0)
            {
                ++second.budgetBacklogEpochs;
                ++total.budgetBacklogEpochs;
                second.backlogEpochAreas.observe(epochAreas);
                second.backlogEpochNs.observe(duration);
            }
        }

        // Aggregate every visit; retain only bounded, deterministic raw samples.
        ++epochSequence;
        if (epochSequence % 256 == 0)
        {
            if (epochs.size() < 4096)
            {
                epochs.push_back(
                    {epochSequence, partition, now - startNs, duration, admissions, epochAreas, remaining, exhausted});
            }
            else
            {
                ++second.epochSamplesDropped;
            }
        }
        epochAreas = 0;
        epochStartNs = 0;
    }
};

class PartitionTickTask : public Task
{
    AreaExecuter* m_executor;
    Area* m_area;

public:
    PartitionTickTask(AreaExecuter* executor, Area* area)
        : m_executor(executor)
        , m_area(area)
    {
    }

    void run() override
    {
        m_executor->enqueue_scheduled(m_area, true);
    }
};

template <size_t Capacity> class PartitionTickBatchTask : public Task
{
    AreaExecuter* m_executor;
    std::array<Area*, Capacity> m_areas{};
    size_t m_count = 0;

public:
    PartitionTickBatchTask(AreaExecuter* executor, Area* const* areas, size_t count)
        : m_executor(executor)
        , m_count(count)
    {
        std::copy_n(areas, count, m_areas.begin());
    }
    void run() override
    {
        for (size_t index = 0; index < m_count; ++index)
            m_executor->enqueue_scheduled(m_areas[index], true);
    }
};

class PartitionTickBatch
{
    AreaExecuter* m_executor;
    std::array<Area*, AreaExecuter::POP_BATCH_SIZE> m_areas{};
    size_t m_count = 0;
    int64_t m_nextTick = 0;

public:
    explicit PartitionTickBatch(AreaExecuter* executor)
        : m_executor(executor)
    {
    }

    void add(Area* area, int64_t nextTick)
    {
        // Only consecutive reservations with the identical absolute deadline share a Task.
        if (m_count != 0 && (m_nextTick != nextTick || m_count == m_areas.size()))
            flush();

        m_nextTick = nextTick;
        m_areas[m_count++] = area;
    }

    void flush()
    {
        if (m_count == 0)
            return;

        Task* task = nullptr;
        if (m_count == 1)
        {
            task = new PartitionTickTask(m_executor, m_areas[0]);
        }
        else if (m_count <= 2)
        {
            task = new PartitionTickBatchTask<2>(m_executor, m_areas.data(), m_count);
        }
        else if (m_count <= 4)
        {
            task = new PartitionTickBatchTask<4>(m_executor, m_areas.data(), m_count);
        }
        else if (m_count <= 8)
        {
            task = new PartitionTickBatchTask<8>(m_executor, m_areas.data(), m_count);
        }
        else if (m_count <= 16)
        {
            task = new PartitionTickBatchTask<16>(m_executor, m_areas.data(), m_count);
        }
        else if (m_count <= 32)
        {
            task = new PartitionTickBatchTask<32>(m_executor, m_areas.data(), m_count);
        }
        else
        {
            task = new PartitionTickBatchTask<AreaExecuter::POP_BATCH_SIZE>(m_executor, m_areas.data(), m_count);
        }

        currentWorker->add_task_at(task, m_nextTick);
        m_count = 0;
    }
};

bool AreaQueue::empty() const
{
    std::shared_lock<std::shared_mutex> guard(m_rwlock);

    return m_areas.empty();
}

std::vector<Area*> AreaQueue::pop_all(size_t count)
{
    std::unique_lock<std::shared_mutex> guard(m_rwlock);

    return pop_n(count);
}

size_t AreaQueue::push(Area* area)
{
    std::unique_lock<std::shared_mutex> guard(m_rwlock);

    if (m_trackWait && m_areas.empty())
        m_waitSince.store(get_now_tick());
    m_areas.emplace_back(area);
    return ++m_size;
}

void AreaQueue::set_wait_tracking(bool enabled)
{
    std::unique_lock<std::shared_mutex> guard(m_rwlock);
    if (m_areas.empty() == false)
        throw std::logic_error("Configure queue wait tracking before publication");

    m_trackWait = enabled;
    m_waitSince.store(0);
}

std::vector<Area*> AreaQueue::pop_n(size_t count)
{
    std::vector<Area*> areas;
    const size_t limit = count ? (std::min)(count, m_areas.size()) : m_areas.size();
    areas.reserve(limit);

    for (size_t i = 0; i < limit; ++i)
    {
        areas.push_back(m_areas.front());
        m_areas.pop_front();
    }
    m_size.fetch_sub(limit);
    if (m_trackWait && limit != 0)
        m_waitSince.store(m_areas.empty() ? 0 : get_now_tick());

    return areas;
}

AreaExecuter::AreaExecuter(size_t partitionRadius, AreaSchedulingOptions options)
{
    setup(partitionRadius);
    configure_scheduling(options);
}

AreaExecuter::~AreaExecuter() = default;

void AreaExecuter::configure_scheduling(AreaSchedulingOptions options)
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    if (m_workers.empty() == false || m_readyAreas.load() != 0 || m_partitionAdmissionAttempts != 0)
        throw std::logic_error("Configure scheduling before publication and Worker attachment");
    if (options.maxSliceMs <= 0 || options.maxOtherWaitMs <= 0 || options.maxAdmissions < PARTITION_ADMISSION_LIMIT
        || options.maxAdmissions > 1024)
        throw std::invalid_argument("Invalid adaptive scheduling limits");

    m_options = options;
    m_admissionLimit = options.unlimited ? (std::numeric_limits<size_t>::max)() : PARTITION_ADMISSION_LIMIT;
    m_schedulingStatistics = {};
    m_schedulingStatistics.maxServiceWaitMs.resize(m_queues.size(), 0);
    for (auto& queue : m_queues)
        queue->set_wait_tracking(options.adaptive || options.unlimited);
}

AreaSchedulingStatistics AreaExecuter::scheduling_statistics() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    return m_schedulingStatistics;
}

void AreaExecuter::configure_profile(size_t sampleStride, size_t seconds, int64_t startTick)
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    if (m_workers.empty() == false)
        throw std::logic_error("Configure Area profiling before attaching Workers");
    if (sampleStride == 0)
    {
        m_profile.reset();
        return;
    }
    if (seconds == 0 || seconds > 3600)
        throw std::invalid_argument("Area profiling requires 1..3600 seconds");

    auto profile = std::make_unique<AreaProfileState>();
    profile->stride = sampleStride;
    profile->startNs = startTick * 1000000;
    profile->seconds.resize(seconds + 2);
    profile->partitions.resize(m_queues.size());
    profile->epochs.reserve(4096);
    m_profile = std::move(profile);
}

std::vector<AreaProfileSecond> AreaExecuter::profile_seconds() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    return m_profile != nullptr ? m_profile->seconds : std::vector<AreaProfileSecond>{};
}

std::vector<AreaProfileEpoch> AreaExecuter::profile_epochs() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    return m_profile != nullptr ? m_profile->epochs : std::vector<AreaProfileEpoch>{};
}

std::vector<AreaProfilePartition> AreaExecuter::profile_partitions() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    return m_profile != nullptr ? m_profile->partitions : std::vector<AreaProfilePartition>{};
}

void AreaExecuter::push(Area* area)
{
    if (area->is_active() == false)
        return;
    if (area->request_schedule() == false)
        return;

    // Only the accepted scheduler owner initializes this reservation before queue publication.
    area->m_nextUpdateTick = get_now_tick();
    area->m_scheduledPartition = get_queue_index(area);
    enqueue_scheduled(area);
}

void AreaExecuter::enqueue_scheduled(Area* area, bool timerCallback)
{
    const size_t idx = area->m_scheduledPartition;
    size_t previousReady = 0, depth = 0;
    if (m_profile != nullptr && m_profile->selected(area))
    {
        const auto callbackNs = profile_now_ns();
        // Publish the sample before the queue entry; consumers use this same lock.
        std::lock_guard<std::mutex> guard(m_partitionLock);
        const auto acquiredNs = profile_now_ns();
        auto& queued = m_profile->queued[area];
        queued = {};
        queued.callbackNs = callbackNs;
        queued.deadlineTick = area->m_nextUpdateTick;
        queued.timerCallback = timerCallback;
        queued.profileLockNs = static_cast<uint64_t>(acquiredNs - callbackNs);
        previousReady = m_readyAreas.fetch_add(1);
        depth = m_queues[idx]->push(area);
        queued.registeredNs = profile_now_ns();
    }
    else
    {
        previousReady = m_readyAreas.fetch_add(1);
        depth = m_queues[idx]->push(area);
    }

    if (previousReady == 0 || depth % POP_BATCH_SIZE == 0)
        wake_workers();
}

void AreaExecuter::attach(Worker* worker)
{
    std::lock_guard<std::mutex> guard(m_partitionLock);

    m_workers.push_back(worker);
}

void AreaExecuter::detach(Worker* worker)
{
    std::lock_guard<std::mutex> guard(m_partitionLock);

    m_workers.erase(std::remove(m_workers.begin(), m_workers.end(), worker), m_workers.end());
}

void AreaExecuter::wake_workers()
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    if (m_workers.empty())
        return;

    const auto queued = m_queues[m_activePartition]->size();
    size_t needed = (queued + POP_BATCH_SIZE - 1) / POP_BATCH_SIZE;
    if (needed == 0 && m_activeRunners == 0 && m_readyAreas.load() != 0)
        needed = 1;

    // The calling Worker will return to on_tick without sleeping when work exists.
    if (needed != 0 && currentWorker != nullptr && currentWorker->is_running()
        && std::find(m_workers.begin(), m_workers.end(), currentWorker) != m_workers.end())
        --needed;

    for (size_t checked = 0; checked < m_workers.size() && needed != 0; ++checked)
    {
        m_wakeIndex %= m_workers.size();
        if (m_workers[m_wakeIndex++]->wake())
            --needed;
    }
}

bool AreaExecuter::has_work() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);

    return m_readyAreas.load() != 0 && can_enter_partition();
}

TickerIdleState AreaExecuter::idle_state() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    const auto ready = m_readyAreas.load();
    if (m_profile != nullptr && currentWorker != nullptr && ready != 0
        && m_partitionAdmissionAttempts >= m_admissionLimit)
    {
        auto& blocked = m_profile->blocked[currentWorker];
        if (blocked == 0)
            blocked = profile_now_ns();
    }
    return {ready, m_activeRunners, ready != 0 && can_enter_partition()};
}

std::vector<uint64_t> AreaExecuter::partition_areas() const
{
    std::lock_guard<std::mutex> guard(m_partitionLock);

    return m_partitionAreas;
}

size_t AreaExecuter::get_queue_index(const Area* area) const
{
    return area->get_partition(m_partitionWidth);
}

bool AreaExecuter::can_enter_partition() const
{
    if (m_partitionAdmissionAttempts >= m_admissionLimit)
        return false;

    // Unlimited admission must not spin on an empty queue while another runner owns the barrier.
    return m_options.unlimited == false || m_activeRunners == 0 || m_queues[m_activePartition]->size() != 0;
}

bool AreaExecuter::try_enter_partition(size_t& partition, std::vector<Area*>& areas)
{
    const auto lockBegin = m_profile != nullptr ? profile_now_ns() : int64_t{0};
    std::lock_guard<std::mutex> guard(m_partitionLock);
    if (m_profile != nullptr)
    {
        const auto acquired = profile_now_ns();
        m_profile->row(acquired).admissionLockNs.observe(static_cast<uint64_t>(acquired - lockBegin));
        auto found = m_profile->blocked.find(currentWorker);
        if (found != m_profile->blocked.end() && found->second != 0)
        {
            const auto now = profile_now_ns();
            m_profile->row(now).blockedReturnNs.observe(static_cast<uint64_t>(now - found->second));
            found->second = 0;
        }
    }

    // Serialize admission and extraction so competing consumers cannot waste an admission on try_lock.
    if (can_enter_partition() == false)
        return false;

    if ((m_options.adaptive || m_options.unlimited) && m_partitionAdmissionAttempts == 0)
    {
        m_partitionStartTick = get_now_tick();
        const auto since = m_queues[m_activePartition]->wait_since();
        if (since != 0)
        {
            auto& maximum = m_schedulingStatistics.maxServiceWaitMs[m_activePartition];
            maximum = (std::max)(maximum, static_cast<uint64_t>((std::max)(int64_t{0}, m_partitionStartTick - since)));
        }
    }
    if (m_profile != nullptr && m_partitionAdmissionAttempts == 0)
        m_profile->epochStartNs = profile_now_ns();
    ++m_partitionAdmissionAttempts;
    ++m_activeRunners;
    partition = m_activePartition;
    areas = m_queues[partition]->pop_all(POP_BATCH_SIZE);
    m_readyAreas.fetch_sub(areas.size());
    if (m_profile != nullptr)
    {
        const auto extractedNs = profile_now_ns();
        for (auto area : areas)
        {
            if (m_profile->selected(area) == false)
                continue;

            const auto found = m_profile->queued.find(area);
            if (found != m_profile->queued.end())
                found->second.extractedNs = extractedNs;
        }
    }
    return true;
}

bool AreaExecuter::extend_partition()
{
    if (m_options.unlimited || m_options.adaptive == false || m_admissionLimit >= m_options.maxAdmissions)
        return false;

    const auto now = get_now_tick();
    if (now - m_partitionStartTick >= m_options.maxSliceMs)
    {
        ++m_schedulingStatistics.sliceStops;
        return false;
    }
    for (size_t id = 0; id < m_queues.size(); ++id)
    {
        if (id == m_activePartition)
            continue;

        const auto since = m_queues[id]->wait_since();
        if (since != 0 && now - since >= m_options.maxOtherWaitMs)
        {
            ++m_schedulingStatistics.waitStops;
            return false;
        }
    }

    m_admissionLimit = (std::min)(m_options.maxAdmissions, m_admissionLimit + PARTITION_ADMISSION_LIMIT);
    ++m_schedulingStatistics.extensions;
    return true;
}

bool AreaExecuter::leave_partition(size_t partition, size_t completedAreas, const AreaProfileSecond* sample)
{
    std::lock_guard<std::mutex> guard(m_partitionLock);
    --m_activeRunners;
    m_partitionAreas[partition] += completedAreas;
    int64_t completionNs = 0;
    if (sample != nullptr)
    {
        completionNs = profile_now_ns();
        auto& row = m_profile->row(completionNs);
        row.batchNs.merge(sample->batchNs);
        row.batchAreas.merge(sample->batchAreas);
        row.batchActorReservations.merge(sample->batchActorReservations);
        row.timerLateNs.merge(sample->timerLateNs);
        row.readyToRunNs.merge(sample->readyToRunNs);
        row.enqueueNs.merge(sample->enqueueNs);
        row.enqueueProfileLockNs.merge(sample->enqueueProfileLockNs);
        row.queueResidenceNs.merge(sample->queueResidenceNs);
        row.extractedToRunNs.merge(sample->extractedToRunNs);
        row.deadlineToRunNs.merge(sample->deadlineToRunNs);
        row.emptyBatches += sample->emptyBatches;
        m_profile->epochAreas += completedAreas;
        if (completedAreas != 0 && m_profile->firstCompletionNs == 0)
            m_profile->firstCompletionNs = completionNs;
    }

    bool extended = false;
    if (m_partitionAdmissionAttempts >= m_admissionLimit && m_queues[partition]->size() != 0)
        extended = extend_partition();

    if (m_activeRunners != 0
        || (m_partitionAdmissionAttempts < m_admissionLimit && m_queues[partition]->empty() == false))
        return extended;

    if (sample != nullptr)
    {
        m_profile->finish_epoch(partition, m_partitionAdmissionAttempts, m_queues[partition]->size(),
                                m_partitionAdmissionAttempts >= m_admissionLimit, completionNs);
    }
    m_activePartition = (partition + 1) % m_queues.size();
    m_partitionAdmissionAttempts = 0;
    m_admissionLimit = m_options.unlimited ? (std::numeric_limits<size_t>::max)() : PARTITION_ADMISSION_LIMIT;
    ++m_partitionChanges;
    if (sample != nullptr)
    {
        if (m_profile->firstCompletionNs != 0)
        {
            m_profile->row(completionNs)
                .epochTailNs.observe(static_cast<uint64_t>(completionNs - m_profile->firstCompletionNs));
        }
        m_profile->firstCompletionNs = 0;
    }
    return true;
}

void AreaExecuter::process_profiled_batch(const std::vector<Area*>& areas, AreaProfileSecond& sample)
{
    if (areas.empty())
    {
        sample.emptyBatches = 1;
        return;
    }

    const auto begin = profile_now_ns();
    uint64_t actors = 0;
    for (auto area : areas)
        actors += static_cast<uint64_t>((std::max)(int32_t{0}, area->m_actorCount.load()));

    PartitionTickBatch pending(this);
    uint64_t skippedTicks = 0;
    for (auto area : areas)
    {
        if (m_profile->selected(area))
        {
            AreaProfileState::Queued queued{};
            {
                std::lock_guard<std::mutex> guard(m_partitionLock);
                const auto found = m_profile->queued.find(area);
                if (found != m_profile->queued.end())
                {
                    queued = found->second;
                    m_profile->queued.erase(found);
                }
            }
            if (queued.timerCallback)
            {
                const auto now = profile_now_ns();
                const auto deadlineNs = queued.deadlineTick * 1000000;
                sample.timerLateNs.observe(
                    static_cast<uint64_t>((std::max)(int64_t{0}, queued.callbackNs - deadlineNs)));
                sample.readyToRunNs.observe(static_cast<uint64_t>((std::max)(int64_t{0}, now - queued.callbackNs)));
                sample.enqueueNs.observe(static_cast<uint64_t>(queued.registeredNs - queued.callbackNs));
                sample.enqueueProfileLockNs.observe(queued.profileLockNs);
                sample.queueResidenceNs.observe(static_cast<uint64_t>(queued.extractedNs - queued.registeredNs));
                sample.extractedToRunNs.observe(static_cast<uint64_t>(now - queued.extractedNs));
                sample.deadlineToRunNs.observe(static_cast<uint64_t>((std::max)(int64_t{0}, now - deadlineNs)));
            }
        }
        skippedTicks += update_area(area, pending);
    }
    pending.flush();

    if (skippedTicks != 0)
        m_skippedTicks.fetch_add(skippedTicks, std::memory_order_relaxed);

    sample.batchNs.observe(static_cast<uint64_t>(profile_now_ns() - begin));
    sample.batchAreas.observe(areas.size());
    sample.batchActorReservations.observe(actors);
}

void AreaExecuter::process_batch(const std::vector<Area*>& areas)
{
    PartitionTickBatch pending(this);
    uint64_t skippedTicks = 0;
    for (auto area : areas)
        skippedTicks += update_area(area, pending);

    pending.flush();

    if (skippedTicks != 0)
        m_skippedTicks.fetch_add(skippedTicks, std::memory_order_relaxed);
}

uint64_t AreaExecuter::update_area(Area* area, PartitionTickBatch& pending)
{
    area->update();
    if (area->finish_schedule() == false)
        return 0;

    const auto plan = next_update_tick(area->m_nextUpdateTick, get_now_tick());
    area->m_nextUpdateTick = (std::max)(get_now_tick(), area->next_actor_tick(plan.nextTick));
    pending.add(area, area->m_nextUpdateTick);
    return plan.skippedTicks;
}

bool AreaExecuter::run()
{
    if (currentWorker == nullptr || m_queues.empty())
        return true;

    size_t partition = 0;
    std::vector<Area*> areas{};
    if (try_enter_partition(partition, areas) == false)
        return true;

    ++m_partitionRuns;

    bool wakeRequested = false;
    if (m_profile != nullptr)
    {
        AreaProfileSecond sample{};
        process_profiled_batch(areas, sample);
        wakeRequested = leave_partition(partition, areas.size(), &sample);
    }
    else
    {
        process_batch(areas);
        wakeRequested = leave_partition(partition, areas.size());
    }
    if (wakeRequested && m_readyAreas.load() != 0)
        wake_workers();

    return areas.empty();
}

bool AreaExecuter::on_tick()
{
    return run();
}

void AreaExecuter::setup(size_t partitionRadius)
{
    m_partitionWidth = 2 * partitionRadius + 1;
    if (m_partitionWidth == 0)
        m_partitionWidth = 1;

    size_t count = m_partitionWidth * m_partitionWidth;
    m_partitionAreas.assign(count, 0);
    for (size_t i = 0; i < count; ++i)
        m_queues.push_back(std::make_unique<AreaQueue>());
}
