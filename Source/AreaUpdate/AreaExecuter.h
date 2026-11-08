
#pragma once

#include <cstdint>

#include <deque>
#include <mutex>
#include <memory>
#include <vector>
#include <shared_mutex>
#include <array>

#include "Area.h"
#include "Worker.h"

class PartitionTickBatch;

struct AreaProfileDistribution
{
    uint64_t count = 0;
    uint64_t sum = 0;
    uint64_t maximum = 0;
    std::array<uint64_t, 32> histogram{};

    void observe(uint64_t value);
    void merge(const AreaProfileDistribution& other);
    uint64_t percentile_upper(unsigned percent) const;
};

struct AreaProfileSecond
{
    AreaProfileDistribution batchNs{};
    AreaProfileDistribution batchAreas{};
    AreaProfileDistribution batchActorReservations{};
    AreaProfileDistribution timerLateNs{};
    AreaProfileDistribution readyToRunNs{};
    AreaProfileDistribution enqueueNs{};
    AreaProfileDistribution enqueueProfileLockNs{};
    AreaProfileDistribution queueResidenceNs{};
    AreaProfileDistribution extractedToRunNs{};
    AreaProfileDistribution admissionLockNs{};
    AreaProfileDistribution deadlineToRunNs{};
    AreaProfileDistribution epochTailNs{};
    AreaProfileDistribution blockedReturnNs{};
    AreaProfileDistribution epochAreas{};
    AreaProfileDistribution epochAdmissions{};
    AreaProfileDistribution epochRemainingAreas{};
    AreaProfileDistribution epochNs{};
    AreaProfileDistribution backlogEpochAreas{};
    AreaProfileDistribution backlogEpochNs{};
    uint64_t budgetExhaustedEpochs = 0;
    uint64_t budgetBacklogEpochs = 0;
    uint64_t epochSamplesDropped = 0;
    uint64_t emptyBatches = 0;
};

struct AreaProfileEpoch
{
    uint64_t sequence = 0;
    size_t partition = 0;
    int64_t elapsedNs = 0;
    uint64_t durationNs = 0;
    size_t admissions = 0;
    size_t areas = 0;
    size_t remainingAreas = 0;
    bool budgetExhausted = false;
};

struct AreaProfilePartition
{
    uint64_t epochs = 0;
    uint64_t admissions = 0;
    uint64_t areas = 0;
    uint64_t budgetExhaustedEpochs = 0;
    uint64_t budgetBacklogEpochs = 0;
    uint64_t remainingAreasSum = 0;
    size_t maxRemainingAreas = 0;
    uint64_t durationNs = 0;
};

struct AreaProfileState;

struct AreaSchedulingOptions
{
    bool adaptive = false;
    int64_t maxSliceMs = 1;
    int64_t maxOtherWaitMs = 10;
    size_t maxAdmissions = 16;
    bool unlimited = true;
};

struct AreaSchedulingStatistics
{
    uint64_t extensions = 0;
    uint64_t sliceStops = 0;
    uint64_t waitStops = 0;
    std::vector<uint64_t> maxServiceWaitMs{};
};

class AreaQueue
{
    mutable std::shared_mutex m_rwlock;
    std::deque<Area*> m_areas;
    std::atomic_size_t m_size{0};
    bool m_trackWait = false;
    std::atomic_int64_t m_waitSince{0};

public:
    AreaQueue() = default;
    ~AreaQueue() = default;

    bool empty() const;
    size_t size() const
    {
        return m_size.load();
    }

    // count == 0 extracts all currently queued Areas.
    std::vector<Area*> pop_all(size_t count = 0);

    size_t push(Area* area);
    void set_wait_tracking(bool enabled);
    int64_t wait_since() const
    {
        return m_waitSince.load();
    }

private:
    std::vector<Area*> pop_n(size_t count);
};

class AreaExecuter : public Ticker
{
    friend class PartitionTickTask;
    template <size_t Capacity> friend class PartitionTickBatchTask;
    friend class PartitionTickBatch;
    static constexpr size_t POP_BATCH_SIZE = 64;
    static constexpr size_t PARTITION_ADMISSION_LIMIT = 8;

    std::vector<std::unique_ptr<AreaQueue>> m_queues;
    mutable std::mutex m_partitionLock;
    std::vector<Worker*> m_workers;
    std::atomic_size_t m_readyAreas{0};
    std::atomic_uint64_t m_partitionRuns{0}, m_partitionChanges{0};
    std::atomic_uint64_t m_skippedTicks{0};
    size_t m_wakeIndex = 0;
    std::vector<uint64_t> m_partitionAreas;
    size_t m_partitionWidth = 1;
    size_t m_activePartition = 0;
    size_t m_activeRunners = 0;
    size_t m_partitionAdmissionAttempts = 0;
    size_t m_admissionLimit = PARTITION_ADMISSION_LIMIT;
    int64_t m_partitionStartTick = 0;
    AreaSchedulingOptions m_options{};
    AreaSchedulingStatistics m_schedulingStatistics{};
    std::unique_ptr<AreaProfileState> m_profile{};

public:
    explicit AreaExecuter(size_t partitionRadius, AreaSchedulingOptions options = {});
    ~AreaExecuter();

    void configure_scheduling(AreaSchedulingOptions options);
    AreaSchedulingStatistics scheduling_statistics() const;

    // Configure before attaching Workers; snapshots are safe under the partition lock.
    void configure_profile(size_t sampleStride, size_t seconds, int64_t startTick);
    std::vector<AreaProfileSecond> profile_seconds() const;
    std::vector<AreaProfileEpoch> profile_epochs() const;
    std::vector<AreaProfilePartition> profile_partitions() const;

    void push(Area* area);
    size_t get_queue_index(const Area* area) const;

    bool run();

    bool on_tick() override;
    bool has_work() const override;
    bool supports_notifications() const override
    {
        return true;
    }
    TickerIdleState idle_state() const override;
    void attach(Worker* worker) override;
    void detach(Worker* worker) override;
    uint64_t partition_runs() const
    {
        return m_partitionRuns.load();
    }
    uint64_t partition_changes() const
    {
        return m_partitionChanges.load();
    }
    std::vector<uint64_t> partition_areas() const;
    uint64_t skipped_ticks() const
    {
        return m_skippedTicks.load();
    }

private:
    // Caller holds m_partitionLock.
    bool can_enter_partition() const;
    bool try_enter_partition(size_t& partition, std::vector<Area*>& areas);
    bool extend_partition();
    bool leave_partition(size_t partition, size_t completedAreas, const AreaProfileSecond* sample = nullptr);
    void process_batch(const std::vector<Area*>& areas);
    uint64_t update_area(Area* area, PartitionTickBatch& pending);
    void process_profiled_batch(const std::vector<Area*>& areas, AreaProfileSecond& sample);
    void enqueue_scheduled(Area* area, bool timerCallback = false);
    void wake_workers();

    void setup(size_t partitionRadius);
};
