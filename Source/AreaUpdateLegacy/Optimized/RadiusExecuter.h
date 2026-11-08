#pragma once

#include "Worker.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct RadiusMask
{
    std::atomic_uint64_t* word = nullptr;
    uint64_t bits = 0;
};

struct RadiusSample
{
    uint64_t acquireNs = 0;
    uint64_t rollbackNs = 0;
    uint64_t releaseNs = 0;
    uint64_t holdBeforeUpdateNs = 0;
    uint64_t attempts = 0;
    uint64_t updates = 0;
};

class RadiusArea;

struct RadiusQueueTrace
{
    int64_t publishedNs = 0, firstProbeNs = 0, acquiredNs = 0;
    uint64_t attemptNs = 0, failures = 0;
    uint64_t holAtPublishNs = 0, holBeforeProbeNs = 0;
    bool holObserved = false;
};

struct RadiusSubmission
{
    RadiusArea* area = nullptr;
    size_t bucket = 0;
};

class RadiusLocks
{
    size_t m_cells = 0;
    std::unique_ptr<std::atomic_uint64_t[]> m_words{};

public:
    explicit RadiusLocks(size_t cells);
    std::vector<RadiusMask> footprint(std::vector<size_t> cells) const;
};

class RadiusArea
{
    std::vector<RadiusMask> m_masks{};

    void release();

public:
    virtual ~RadiusArea() = default;
    // Configure the immutable footprint, including the center cell, before publication.
    void set_footprint(std::vector<RadiusMask> masks);
    bool try_acquire(RadiusSample* sample = nullptr);
    void update(RadiusSample* sample = nullptr);

private:
    virtual void do_update() = 0;
};

class RadiusExecuter : public Ticker
{
    static constexpr size_t BATCH_SIZE = 64;
    struct Queue
    {
        std::mutex mutex;
        std::deque<RadiusArea*> areas{};
        int64_t holSinceNs = 0;
        uint64_t holNs = 0, visits = 0, headFailures = 0, popped = 0;
        std::atomic_uint64_t lockMisses{0};
        uint64_t hol_clock(int64_t now) const
        {
            return holNs + (holSinceNs != 0 ? static_cast<uint64_t>((std::max)(int64_t{0}, now - holSinceNs)) : 0);
        }
    };
    struct LocalState
    {
        const RadiusExecuter* owner = nullptr;
        size_t next = 0;
        uint64_t generation = 0;
        bool progressed = false;
        bool attached = false;
        bool pendingWake = false;
    };
    std::vector<std::unique_ptr<Queue>> m_queues{};
    std::atomic_size_t m_ready{0}, m_runners{0};
    std::atomic_uint64_t m_generation{0}, m_failures{0}, m_updates{0};
    std::mutex m_workersLock;
    std::vector<Worker*> m_workers{};
    size_t m_batchSize = BATCH_SIZE;
    size_t m_profileStride = 0;
    std::atomic_uint64_t m_passes{0}, m_batches{0}, m_wakeCalls{0}, m_wakeTargets{0}, m_wakeAccepted{0};
    std::atomic_uint64_t m_wakeNs{0}, m_wakeSamples{0};
    std::atomic_uint64_t m_pushCalls{0}, m_pushLocks{0}, m_pushAreas{0};
    std::mutex m_profileLock;
    RadiusSample m_profile{};
    std::unordered_map<RadiusArea*, RadiusQueueTrace> m_queueTraces{};
    bool m_queueDiagnostics = false;
    std::function<void()> m_afterBatch{};
    bool m_fastBucket = true, m_mergeNotifications = true;
    std::atomic_uint64_t m_fastBatches{0}, m_deferredWakes{0}, m_notificationFlushes{0};
    std::atomic_uint64_t m_emptyWakes{0};
#ifdef AREAUPDATE_TEST_HOOKS
    void (*m_publishHook)(void*) = nullptr;
    void* m_publishContext = nullptr;
    void (*m_groupHook)(void*) = nullptr;
    void* m_groupContext = nullptr;
#endif

    static LocalState& local_state();
    void wake_workers();
    void notify_publication(bool emptyQueue);
    void flush_notifications(LocalState& state);
    void trace_publication(RadiusArea* area, const Queue& queue);

public:
    explicit RadiusExecuter(size_t queues);
    // Configure only before Workers start; read diagnostics after join.
    void configure(size_t batchSize, size_t profileStride, std::function<void()> afterBatch = {});
    void configure_publication(bool fastBucket, bool mergeNotifications);
    void configure_queue_diagnostics(bool enabled);
    // Configure before publication/Worker attachment; require one queued/in-flight reservation per Area.
    // The map stays immutable while Workers run.
    void configure_queue_trace(RadiusArea* area, bool enabled);
    // Trace fields are owned by the queue, then by the acquired Area; read after join or from that owner.
    RadiusQueueTrace* queue_trace(RadiusArea* area);
    uint64_t deferred_wakes() const
    {
        return m_deferredWakes.load();
    }
    uint64_t notification_flushes() const
    {
        return m_notificationFlushes.load();
    }
    void print_profile() const;
    void push(RadiusArea* area, size_t bucket);
    // Preserve submission order within each bucket; at most 64 entries per publication.
    void push_batch(const RadiusSubmission* submissions, size_t count);
#ifdef AREAUPDATE_TEST_HOOKS
    void set_publish_hook(void (*hook)(void*), void* context)
    {
        m_publishHook = hook;
        m_publishContext = context;
    }
    void set_group_hook(void (*hook)(void*), void* context)
    {
        m_groupHook = hook;
        m_groupContext = context;
    }
#endif
    bool on_tick() override;
    bool has_work() const override;
    TickerIdleState idle_state() const override;
    bool supports_notifications() const override
    {
        return true;
    }
    void attach(Worker* worker) override;
    void detach(Worker* worker) override;
    uint64_t failures() const
    {
        return m_failures.load();
    }
    uint64_t updates() const
    {
        return m_updates.load();
    }
};
