#include "Worker.h"
#include <functional>
#include "WakeSignal.h"
#include "AreaExecuter.h"
#include "UpdateTickPlan.h"
#include <stdexcept>
#include <vector>
#include <cstdio>

namespace
{
    void require_at(bool value, int line)
    {
        if (value == false)
        {
            std::fprintf(stderr, "Runtime regression failed at line %d\n", line);
            throw std::runtime_error("Runtime regression failed");
        }
    }
#define require(value) require_at((value), __LINE__)
    void wake_signal()
    {
        using Clock = std::chrono::steady_clock;
        WakeSignal signal;
        signal.signal();
        signal.signal();
        require(signal.wait_until(Clock::now() + std::chrono::seconds(1)));
        require(signal.wait_until(Clock::now() + std::chrono::milliseconds(2)) == false);

        WakeSignal acknowledged;
        std::atomic_bool success{true};
        std::thread consumer(
            [&]
            {
                for (size_t i = 0; i < 1000; ++i)
                {
                    if (signal.wait_until(Clock::now() + std::chrono::seconds(2)) == false)
                    {
                        success = false;
                        break;
                    }
                    acknowledged.signal();
                }
            });

        for (size_t i = 0; i < 1000; ++i)
        {
            signal.signal();
            if (acknowledged.wait_until(Clock::now() + std::chrono::seconds(2)) == false)
            {
                success = false;
                break;
            }
        }

        consumer.join();
        require(success.load());
    }

    template <class Predicate> bool wait_until(Predicate predicate, int timeout = 2000)
    {
        const auto deadline = get_now_tick() + timeout;

        while (predicate() == false && get_now_tick() < deadline)
            std::this_thread::yield();

        return predicate();
    }

    class ProbeTask : public Task
    {
        std::function<void()> m_function;

    public:
        explicit ProbeTask(std::function<void()> function = [] {})
            : m_function(std::move(function))
        {
        }
        void run() override
        {
            m_function();
        }
    };

    void wait_return_diagnostics()
    {
        WaitReturnBucket bucket{};
        bucket.observe(0, 0);
        bucket.observe(5000000, 6);
        bucket.observe(123000000, 124);
        require(bucket.returns == 3 && bucket.lateTimerReturns == 2 && bucket.lateTimerMs == 130);
        require(bucket.overshoot_percentile_ms(99) == 100 && bucket.maxOvershootNs == 123000000);
        const auto copy = bucket;
        bucket.merge(copy);
        require(bucket.returns == 6 && bucket.lateTimerMs == 260 && bucket.overshootNs == 256000000);

        Worker worker;
        worker.set_wait_return_diagnostics(true);
        worker.start(nullptr, 0);
        bool liveReadRejected = false;
        try
        {
            worker.wait_return_statistics();
        }
        catch (const std::logic_error&)
        {
            liveReadRejected = true;
        }
        const bool initiallyWaiting = wait_until(
            [&]
            {
                return worker.statistics().waitCount != 0;
            });
        worker.add_task(new ProbeTask, 2000);
        const bool timedWaiting = wait_until(
            [&]
            {
                return worker.statistics().waitCount >= 2;
            });
        std::atomic_bool completed{false};
        worker.add_task(new ProbeTask(
                            [&]
                            {
                                completed = true;
                            }),
                        10);
        const bool finished = wait_until(
            [&]
            {
                return completed.load();
            });
        worker.stop();
        worker.join();
        const auto stats = worker.statistics();
        const auto returns = worker.wait_return_statistics();
        uint64_t count = 0, late = 0, lateMs = 0, notifications = 0;
        for (const auto& outcome : returns.outcomes)
        {
            count += outcome.returns;
            late += outcome.lateTimerReturns;
            lateMs += outcome.lateTimerMs;
            notifications += outcome.notificationSamples;
        }
        require(liveReadRejected && initiallyWaiting && timedWaiting && finished && returns.enabled);
        require(count == stats.waitCount && late == stats.sleepPastTimerCount && lateMs == stats.sleepPastTimerMs);
        require(notifications == stats.notifiedWaits);
        require(returns.outcomes[static_cast<size_t>(WaitReturnKind::Timeout)].returns != 0);
        require(returns.outcomes[static_cast<size_t>(WaitReturnKind::Signaled)].returns != 0);
        require(returns.outcomes[static_cast<size_t>(WaitReturnKind::Untimed)].returns != 0);

        Worker queued;
        queued.add_task(new ProbeTask);
        bool rejectedPending = false;
        try
        {
            queued.set_wait_return_diagnostics(true);
        }
        catch (const std::logic_error&)
        {
            rejectedPending = true;
        }
        require(rejectedPending && queued.wait_return_statistics().enabled == false);
    }

    void optional_worker_diagnostics()
    {
        Worker queued;
        queued.add_task(new ProbeTask);
        bool rejectedPending = false;
        try
        {
            queued.set_runtime_diagnostics(false);
        }
        catch (const std::logic_error&)
        {
            rejectedPending = true;
        }
        Worker traced;
        traced.set_timer_trace_threshold(1);
        bool rejectedDisable = false;
        try
        {
            traced.set_runtime_diagnostics(false);
        }
        catch (const std::logic_error&)
        {
            rejectedDisable = true;
        }
        Worker worker;
        worker.set_runtime_diagnostics(false);
        bool rejectedTrace = false;
        try
        {
            worker.set_timer_trace_threshold(1);
        }
        catch (const std::logic_error&)
        {
            rejectedTrace = true;
        }
        std::atomic_int completed{0};
        worker.start(nullptr, 0);
        require(worker.add_task(new ProbeTask(
            [&]
            {
                ++completed;
            })));
        require(worker.add_task(new ProbeTask(
                                    [&]
                                    {
                                        ++completed;
                                    }),
                                10));
        const bool finished = wait_until(
            [&]
            {
                return completed.load() == 2;
            });
        worker.stop();
        worker.join();
        const auto stats = worker.statistics();
        require(rejectedPending && rejectedDisable && rejectedTrace && finished && stats.pendingTasks == 0);
        require(stats.immediateTasks == 1 && stats.timerTasks == 1 && stats.maxQueueDepth == 0);
        require(stats.maxTimerDelayMs == 0 && stats.maxQueueDelayMs == 0);
    }

    void fixed_object_period()
    {
        require(UpdateTickPlan::INTERVAL_MS == 50);
        const auto first = next_update_tick(1000, 1017);
        require(first.nextTick == 1050 && first.skippedTicks == 0);
        const auto boundary = next_update_tick(1000, 1050);
        require(boundary.nextTick == 1100 && boundary.skippedTicks == 1);
        const auto stalled = next_update_tick(1000, 1173);
        require(stalled.nextTick == 1200 && stalled.skippedTicks == 3);

        int64_t planned = 1000;
        for (int64_t i = 1; i <= 1000; ++i)
        {
            const auto next = next_update_tick(planned, planned + 17);
            require(next.nextTick == 1000 + i * 50 && next.skippedTicks == 0);
            planned = next.nextTick;
        }

        Worker worker;
        std::atomic_bool ran{false}, early{false};
        const auto due = get_now_tick() + 30;
        const bool accepted = worker.add_task_at(new ProbeTask(
                                                     [&]
                                                     {
                                                         early = get_now_tick() < due;
                                                         ran = true;
                                                     }),
                                                 due);
        worker.start(nullptr, 0);
        const bool finished = wait_until(
            [&]
            {
                return ran.load();
            });
        worker.stop();
        worker.join();

        require(accepted && finished && early.load() == false);
    }

    void timer_trace_storage()
    {
        TimerTraceBuffer buffer;
        for (uint64_t i = 0; i < 100; ++i)
        {
            TimerDelayTrace event{};
            event.actorTag = i;
            event.delayMs = 50 + i;
            buffer.observe(event.delayMs);
            buffer.retain(event);
        }
        TimerDelayTrace worst{};
        worst.delayMs = 500;
        buffer.observe(worst.delayMs);
        buffer.retain(worst);
        const auto summary = buffer.summary();
        const auto events = buffer.events();
        require(summary.observed == 101 && summary.slow == 101 && summary.retained == 32);
        require(summary.histogram[200] == 1);
        uint64_t maximum = 0;
        for (const auto& event : events)
        {
            require(event.delayMs >= 119);
            maximum = (std::max)(maximum, event.delayMs);
        }
        require(maximum == 500);

        Worker worker;
        worker.set_timer_trace_threshold(10);
        std::atomic_bool ran{false};
        const auto due = get_now_tick() + 10;
        worker.add_task_at(new ProbeTask(
                               [&]
                               {
                                   ran = true;
                               }),
                           due);
        worker.add_task(new ProbeTask(
            []
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }));
        worker.start(nullptr, 0);
        const bool finished = wait_until(
            [&]
            {
                return ran.load();
            });
        worker.stop();
        worker.join();

        const auto traces = worker.timer_traces();
        require(finished && traces.size() == 1);
        require(traces.front().deadlineTick == due && traces.front().delayMs >= 50);
        require(traces.front().requestEndTick - traces.front().requestBeginTick >= 50);
        require(worker.timer_trace_summary().observed == 1);
    }

    void timer_resolution_boundary()
    {
        require(WheelTimer::slot_count() == 256);
        WheelTimer timer(0);
        const auto resolution = WheelTimer::resolution_ms();
        const auto rounded = ((7 + resolution - 1) / resolution) * resolution;
        timer.push_at(new ProbeTask(), 7);
        require(timer.expired_tasks(rounded - 1).empty());
        const auto expired = timer.expired_tasks(rounded);
        require(expired.size() == 1);
        delete expired.front();
    }

    void timer_boundaries()
    {
        WheelTimer timer(0);
        const int64_t deadlines[] = {0, 1, 4, 5, 6, 1275, 1276, 1280, 1281, 5000};
        int64_t observed = 0;
        size_t executed = 0;

        for (auto due : deadlines)
        {
            timer.push_at(new ProbeTask(
                              [&, due]
                              {
                                  require(observed >= due);
                                  ++executed;
                              }),
                          due);
        }

        for (observed = 0; observed <= 5005; ++observed)
        {
            for (auto task : timer.expired_tasks(observed))
            {
                task->run();
                delete task;
            }
        }

        require(executed == 10 && timer.size() == 0);

        for (int i = 0; i < 2000; ++i)
            timer.push_at(new ProbeTask(), 5010);

        size_t count = 0;
        while (timer.size())
        {
            auto tasks = timer.expired_tasks(1000000000000LL, 128);
            require(tasks.empty() == false && tasks.size() <= 128);
            count += tasks.size();

            for (auto task : tasks)
                delete task;
        }

        require(count == 2000);

        {
            WheelTimer mixed(0);
            bool shortExecuted = false;

            for (int i = 0; i < 2000; ++i)
                mixed.push_at(new ProbeTask(), 5000);

            mixed.push_at(new ProbeTask(
                              [&]
                              {
                                  shortExecuted = true;
                              }),
                          50);

            auto batch = mixed.expired_tasks(5000, 128);
            for (auto task : batch)
            {
                task->run();
                delete task;
            }

            require(shortExecuted);
        }

        // A huge empty jump must not iterate over every elapsed millisecond.
        timer.expired_tasks(2000000000000LL);
        timer.push_at(new ProbeTask(), 2000000000007LL);
        require(timer.expired_tasks(2000000000006LL).empty());

        auto tasks = timer.expired_tasks(2000000000010LL);
        require(tasks.size() == 1);

        delete tasks.front();
    }

    void interruptible_wait()
    {
        Worker worker;
        std::atomic_bool immediate{false}, timer{false}, early{false};
        worker.start(nullptr, 0);
        const bool parked = wait_until(
            [&]
            {
                return worker.statistics().waitCount != 0;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const auto idleWaits = worker.statistics().waitCount;

        worker.add_task_at(new ProbeTask(), (std::numeric_limits<int64_t>::max)() - 1);
        const bool registered = wait_until(
            [&]
            {
                return worker.statistics().maxQueueDepth != 0 && worker.statistics().waitCount > idleWaits;
            });
        const auto due = get_now_tick() + 20;
        worker.add_task_at(new ProbeTask(
                               [&]
                               {
                                   early = get_now_tick() < due;
                                   timer = true;
                               }),
                           due);
        worker.add_task(new ProbeTask(
            [&]
            {
                immediate = true;
            }));
        const bool completed = wait_until(
            [&]
            {
                return immediate.load() && timer.load();
            });

        worker.stop();
        worker.join();
        const auto stats = worker.statistics();
        require(parked && registered && completed && early.load() == false);
        require(idleWaits == 1 && stats.signalWaits != 0 && stats.notifiedWaits != 0);
        require(stats.cancelledTasks == 1 && stats.pendingTasks == 0);
    }

    void retained_worker_wake()
    {
#ifdef AREAUPDATE_TEST_HOOKS
        struct WaitGate
        {
            Worker* worker = nullptr;
            std::atomic_bool submitted{false}, ran{false};
        } gate;
        Worker worker;
        gate.worker = &worker;
        worker.set_polling_wait(false);
        worker.set_wait_hook(
            [](void* context)
            {
                auto& state = *static_cast<WaitGate*>(context);
                if (state.submitted.exchange(true))
                    return;

                state.worker->add_task(new ProbeTask(
                    [&state]
                    {
                        state.ran = true;
                    }));
            },
            &gate);
        worker.start(nullptr, 0);
        const bool completed = wait_until(
            [&]
            {
                return gate.ran.load();
            });
        worker.stop();
        worker.join();

        require(completed && worker.statistics().notifiedWaits != 0);
#endif
    }

    void defer_later_timer_wake()
    {
        Worker worker;
        worker.set_polling_wait(false);
        std::atomic_bool ran{false};
        worker.add_task(new ProbeTask(), 10000);
        worker.start(nullptr, 0);
        const bool parked = wait_until(
            [&]
            {
                return worker.statistics().waitCount != 0;
            });
        const auto waits = worker.statistics().waitCount;

        worker.add_task(new ProbeTask(), 20000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool deferred = worker.statistics().waitCount == waits;
        worker.add_task(new ProbeTask(
            [&]
            {
                ran = true;
            }));
        const bool completed = wait_until(
            [&]
            {
                return ran.load();
            });
        worker.stop();
        worker.join();

        require(parked && deferred && completed && worker.statistics().cancelledTasks == 2);
    }

    void unnotified_ticker()
    {
        class PollTicker final : public Ticker
        {
        public:
            std::atomic_size_t ticks{0};
            bool on_tick() override
            {
                ++ticks;
                return true;
            }
            bool has_work() const override
            {
                return false;
            }
        } ticker;
        Worker worker;
        worker.set_polling_wait(false);
        worker.start(&ticker, 0);
        const bool progressed = wait_until(
            [&]
            {
                return ticker.ticks.load() >= 3;
            });
        worker.stop();
        worker.join();

        require(progressed);
    }

    void queue_and_wakeup(bool polling)
    {
        Worker worker;
        worker.set_polling_wait(polling);
        std::atomic_int processed{0};
        std::atomic_bool ordered{true}, early{false};
        int previous[4] = {-1, -1, -1, -1};

        worker.start(nullptr, 0);

        // An earlier external request must interrupt a long timer wait.
        require(worker.add_task(new ProbeTask(), 10000));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        std::vector<std::thread> producers;
        for (int producer = 0; producer < 4; ++producer)
        {
            producers.emplace_back(
                [&, producer]
                {
                    for (int sequence = 0; sequence < 250; ++sequence)
                    {
                        worker.add_task(new ProbeTask(
                            [&, producer, sequence]
                            {
                                if (previous[producer] + 1 != sequence)
                                    ordered = false;

                                previous[producer] = sequence;
                                ++processed;
                            }));
                    }
                });
        }

        for (auto& producer : producers)
            producer.join();

        const auto due = get_now_tick() + 15;
        require(worker.add_task(new ProbeTask(
                                    [&, due]
                                    {
                                        early = get_now_tick() < due;
                                        ++processed;
                                    }),
                                15));

        const auto deadline = get_now_tick() + 3000;
        while (processed != 1001 && get_now_tick() < deadline)
            std::this_thread::yield();

        worker.stop();
        worker.join();

        const auto stats = worker.statistics();
        require(processed == 1001 && ordered && early == false);
        require(stats.immediateTasks == 1000 && stats.timerTasks == 1 && stats.cancelledTasks == 1);
        require(stats.pendingTasks == 0 && stats.pendingTimers == 0 && stats.waitCount > 0);
        require(stats.sleepReadySamples + stats.sleepBlockedSamples <= stats.waitCount);
        require(stats.sleepPastTimerCount <= stats.waitCount && stats.sleepWakeRequests <= stats.waitCount);

        require(worker.add_task(new ProbeTask()) == false);
    }

    void area_profile()
    {
        AreaProfileDistribution values{};
        values.observe(0);
        values.observe(1);
        values.observe(3);
        AreaProfileDistribution extra{};
        extra.observe(9);
        values.merge(extra);
        require(values.count == 4 && values.sum == 13 && values.maximum == 9);
        require(values.percentile_upper(50) == 1 && values.percentile_upper(99) == 9);
        extra.observe(uint64_t{1} << 40);
        require(extra.percentile_upper(99) == (uint64_t{1} << 40));

        struct ProfileArea : Area
        {
            std::atomic_int count{0};
            explicit ProfileArea(int x)
                : Area(x, 0)
            {
            }
            void do_update() override
            {
                ++count;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        AreaSchedulingOptions fixedOptions{};
        fixedOptions.unlimited = false;
        AreaExecuter executor(2, fixedOptions);
        require(executor.profile_seconds().empty());
        executor.configure_profile(1, 2, get_now_tick());
        std::vector<std::unique_ptr<ProfileArea>> areas{};
        for (int i = 0; i < 16; ++i)
        {
            areas.push_back(std::make_unique<ProfileArea>(i * 5));
            areas.back()->activate();
            executor.push(areas.back().get());
        }
        Worker workers[2];
        workers[0].start(&executor, 0);
        workers[1].start(&executor, 1);
        const bool completed = wait_until(
            [&]
            {
                return areas.front()->count >= 3;
            });
        workers[0].stop();
        workers[1].stop();
        workers[0].join();
        workers[1].join();

        AreaProfileDistribution timer{}, ready{}, total{}, batches{}, sizes{}, enqueue{}, residence{}, extracted{};
        AreaProfileDistribution epochAreas{}, admissions{};
        for (const auto& row : executor.profile_seconds())
        {
            timer.merge(row.timerLateNs);
            ready.merge(row.readyToRunNs);
            total.merge(row.deadlineToRunNs);
            batches.merge(row.batchNs);
            sizes.merge(row.batchAreas);
            enqueue.merge(row.enqueueNs);
            residence.merge(row.queueResidenceNs);
            extracted.merge(row.extractedToRunNs);
            epochAreas.merge(row.epochAreas);
            admissions.merge(row.epochAdmissions);
            require(row.budgetBacklogEpochs <= row.budgetExhaustedEpochs);
        }
        require(completed && batches.count != 0 && batches.count == sizes.count);
        require(timer.count > 0 && timer.count == ready.count && timer.count == total.count);
        require(total.sum == timer.sum + ready.sum);
        require(ready.count == enqueue.count && ready.count == residence.count && ready.count == extracted.count);
        require(ready.sum == enqueue.sum + residence.sum + extracted.sum);
        require(sizes.sum >= timer.count + 16);
        require(epochAreas.count > 0 && epochAreas.count == admissions.count && admissions.maximum <= 8);
        require(epochAreas.maximum <= 8 * 64);
        uint64_t partitionEpochs = 0, partitionAreas = 0;
        for (const auto& value : executor.profile_partitions())
        {
            partitionEpochs += value.epochs;
            partitionAreas += value.areas;
            require(value.budgetBacklogEpochs <= value.budgetExhaustedEpochs);
        }
        require(partitionEpochs == epochAreas.count && partitionAreas == epochAreas.sum);
    }

    void owner_timer_submission()
    {
        Worker worker;
        std::atomic_bool completed{false}, early{false}, accepted{true};
        worker.set_timer_trace_threshold(1);
        worker.start(nullptr, 0);
        worker.add_task(new ProbeTask(
            [&]
            {
                const auto firstDue = get_now_tick() + 15;
                accepted = worker.add_task_at(new ProbeTask(
                                                  [&, firstDue]
                                                  {
                                                      early = get_now_tick() < firstDue;
                                                      const auto secondDue = get_now_tick() + 15;
                                                      if (worker.add_task_at(new ProbeTask(
                                                                                 [&, secondDue]
                                                                                 {
                                                                                     if (get_now_tick() < secondDue)
                                                                                         early = true;
                                                                                     completed = true;
                                                                                 }),
                                                                             secondDue)
                                                          == false)
                                                          accepted = false;
                                                  }),
                                              firstDue);
                if (worker.add_task(new ProbeTask(), 60000) == false)
                    accepted = false;
            }));

        const bool finished = wait_until(
            [&]
            {
                return completed.load();
            });
        worker.stop();
        worker.join();

        const auto stats = worker.statistics();
        require(finished && accepted && early == false);
        require(stats.immediateTasks == 1 && stats.timerTasks == 2 && stats.cancelledTasks == 1);
        require(stats.timerPhaseNs + stats.requestPhaseNs + stats.tickerPhaseNs > 0);
        require(stats.timerPhaseNs + stats.requestPhaseNs + stats.tickerPhaseNs <= stats.busyNs);
        require(stats.blockedIdleNs <= stats.idleNs);
        require(stats.pendingTasks == 0 && stats.pendingTimers == 0);
    }

    void stop_during_submission()
    {
        Worker worker;
        worker.set_polling_wait(false);
        std::atomic_bool go{false};
        std::atomic_uint64_t accepted{0};

        worker.start(nullptr, 0);

        std::vector<std::thread> producers;
        for (int producer = 0; producer < 4; ++producer)
        {
            producers.emplace_back(
                [&]
                {
                    while (go == false)
                        std::this_thread::yield();

                    for (int i = 0; i < 500; ++i)
                    {
                        const bool result =
                            i % 2 ? worker.add_task(new ProbeTask(), 5000) : worker.add_task(new ProbeTask());
                        if (result)
                            ++accepted;
                    }
                });
        }

        go = true;
        while (accepted.load() == 0)
            std::this_thread::yield();

        worker.stop();
        for (auto& producer : producers)
            producer.join();
        worker.join();

        const auto stats = worker.statistics();
        require(stats.immediateTasks + stats.timerTasks + stats.cancelledTasks == accepted);
        require(stats.pendingTasks == 0 && stats.pendingTimers == 0);
    }

    void accepted_submission_during_stop()
    {
#ifdef AREAUPDATE_TEST_HOOKS
        struct Gate
        {
            std::atomic_bool entered{false}, release{false};
        } gate;

        Worker worker;
        worker.set_submission_hook(
            [](void* context)
            {
                auto& state = *static_cast<Gate*>(context);
                state.entered = true;

                while (state.release == false)
                    std::this_thread::yield();
            },
            &gate);
        worker.set_polling_wait(false);
        worker.start(nullptr, 0);

        std::atomic_bool accepted{false}, stopping{false}, joined{false};

        std::thread producer(
            [&]
            {
                accepted = worker.add_task(new ProbeTask());
            });

        const bool entered = wait_until(
            [&]
            {
                return gate.entered.load();
            });

        std::thread stopper(
            [&]
            {
                worker.stop();
                stopping = true;
                worker.join();
                joined = true;
            });
        const bool stopped = wait_until(
            [&]
            {
                return stopping.load();
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool waited = joined.load() == false;

        gate.release = true;
        producer.join();
        stopper.join();

        const auto stats = worker.statistics();
        require(entered && stopped && waited && accepted && joined);
        require(stats.cancelledTasks == 1 && stats.pendingTasks == 0);
#endif
    }

    class CallbackArea : public Area
    {
        std::function<void()> m_callback;

    public:
        CallbackArea(int x, int y, std::function<void()> callback)
            : Area(x, y)
            , m_callback(std::move(callback))
        {
            activate();
        }
        void do_update() override
        {
            m_callback();
        }
    };

    void adaptive_admission()
    {
        for (int scenario = 0; scenario < 4; ++scenario)
        {
            AreaSchedulingOptions options{};
            options.adaptive = true;
            options.maxSliceMs = scenario >= 2 ? 1 : 1000;
            options.maxOtherWaitMs = scenario == 1 || scenario == 3 ? 1 : 1000;
            options.maxAdmissions = 16;
            options.unlimited = scenario == 3;
            AreaExecuter executor(2, options);
            std::atomic_int started{0}, observed{0};
            std::atomic_bool done{false};
            std::vector<std::unique_ptr<CallbackArea>> hot{};
            for (int i = 0; i < 1025; ++i)
            {
                auto update = [&, i]
                {
                    if (scenario >= 2 && i == 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));

                    ++started;
                    hot[i]->set_active(false);
                };
                hot.push_back(std::make_unique<CallbackArea>(i * 5, 0, std::move(update)));
                executor.push(hot.back().get());
            }
            CallbackArea cold(1, 0,
                              [&]
                              {
                                  observed = started.load();
                                  done = true;
                                  cold.set_active(false);
                              });
            executor.push(&cold);
            if (scenario == 1 || scenario == 3)
                std::this_thread::sleep_for(std::chrono::milliseconds(3));

            Worker worker;
            worker.start(&executor, 0);
            const bool finished = wait_until(
                [&]
                {
                    return done.load();
                });
            worker.stop();
            worker.join();

            const auto stats = executor.scheduling_statistics();
            const int expected = scenario == 3 ? 1025 : (scenario == 0 ? 1024 : 512);
            require(finished && observed == expected);
            if (scenario == 0)
                require(stats.extensions > 0);
            if (scenario == 1)
                require(stats.waitStops > 0);
            if (scenario == 2)
                require(stats.sliceStops > 0);
            if (scenario == 3)
                require(stats.extensions == 0 && stats.sliceStops == 0 && stats.waitStops == 0);
        }
    }

    void adaptive_partition_barrier(bool unlimited)
    {
        AreaSchedulingOptions options{};
        options.adaptive = true;
        options.maxSliceMs = 1000;
        options.maxOtherWaitMs = 1000;
        options.maxAdmissions = 16;
        options.unlimited = unlimited;
        AreaExecuter executor(2, options);
        std::atomic_int started{0}, active{0}, overlaps{0};
        std::atomic_bool release{false}, coldDone{false};
        std::vector<std::unique_ptr<CallbackArea>> hot{};
        for (int i = 0; i < 1025; ++i)
        {
            auto update = [&, i]
            {
                ++active;
                ++started;
                if (i == 0)
                {
                    while (release.load() == false)
                        std::this_thread::yield();
                }
                hot[i]->set_active(false);
                --active;
            };
            hot.push_back(std::make_unique<CallbackArea>(i * 5, 0, std::move(update)));
            executor.push(hot.back().get());
        }
        CallbackArea cold(1, 0,
                          [&]
                          {
                              if (active.load() != 0)
                                  ++overlaps;
                              cold.set_active(false);
                              coldDone = true;
                          });
        executor.push(&cold);
        Worker workers[2];
        workers[0].start(&executor, 0);
        workers[1].start(&executor, 1);

        const bool extended = wait_until(
            [&]
            {
                // The blocked first batch still owns 63 Areas that have not started.
                return started >= (unlimited ? 1025 - 64 + 1 : 900);
            });
        const bool blocked = coldDone.load() == false;
        bool parked = true;
        if (unlimited)
        {
            const auto state = executor.idle_state();
            const auto visits = executor.partition_runs();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            parked = state.runnable == false && executor.has_work() == false && executor.partition_runs() == visits;
        }
        release = true;
        const bool finished = wait_until(
            [&]
            {
                return coldDone.load();
            });
        workers[0].stop();
        workers[1].stop();
        workers[0].join();
        workers[1].join();

        require(extended);
        require(blocked);
        require(parked);
        require(finished);
        require(overlaps == 0);
        require(unlimited || executor.scheduling_statistics().extensions > 0);
    }

    void fixed_area_period()
    {
        AreaExecuter executor(2);
        Worker worker;
        std::vector<int64_t> plannedTicks, observedTicks, completedTicks;
        std::atomic_bool inactiveDone{false}, reactivatedDone{false};

        CallbackArea area(0, 0,
                          [&]
                          {
                              plannedTicks.push_back(area.get_scheduled_tick());
                              observedTicks.push_back(get_now_tick());
                              if (plannedTicks.size() == 1)
                                  std::this_thread::sleep_for(std::chrono::milliseconds(80));
                              completedTicks.push_back(get_now_tick());

                              const auto count = plannedTicks.size();
                              if (count == 4 || count == 5)
                              {
                                  area.set_active(false);
                                  // This request runs after on_tick has completed finish_schedule.
                                  currentWorker->add_task(new ProbeTask(
                                      [&, count]
                                      {
                                          if (count == 4)
                                          {
                                              inactiveDone = true;
                                          }
                                          else
                                          {
                                              reactivatedDone = true;
                                          }
                                      }));
                              }
                          });
        area.set_wake(
            [&](Area* value)
            {
                executor.push(value);
            });
        worker.set_polling_wait(false);
        worker.start(&executor, 0);
        executor.push(&area);
        const bool firstCycle = wait_until(
            [&]
            {
                return inactiveDone.load();
            },
            5000);

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const auto reactivationBegin = get_now_tick();
        area.activate();
        const bool secondCycle = wait_until(
            [&]
            {
                return reactivatedDone.load();
            },
            5000);
        worker.stop();
        worker.join();

        require(firstCycle && secondCycle && plannedTicks.size() == 5 && executor.skipped_ticks() >= 1);
        require(plannedTicks[1] - plannedTicks[0] >= 100);
        for (size_t i = 0; i < plannedTicks.size(); ++i)
            require(observedTicks[i] >= plannedTicks[i]);
        for (size_t i = 0; i < 3; ++i)
        {
            const auto expected = next_update_tick(plannedTicks[i], completedTicks[i]);
            require(plannedTicks[i + 1] >= expected.nextTick);
            require((plannedTicks[i + 1] - plannedTicks[0]) % UpdateTickPlan::INTERVAL_MS == 0);
        }
        require(plannedTicks[4] >= reactivationBegin && plannedTicks[4] > plannedTicks[3] + 50);
    }

    void partition_exclusion()
    {
        AreaExecuter executor(2);
        Worker workers[2];
        std::atomic_bool entered{false}, release{false}, running{false}, secondRan{false}, overlap{false};
        std::atomic_size_t owner{0};

        CallbackArea first(0, 0,
                           [&]
                           {
                               owner = currentWorker->get_id();
                               running = true;
                               entered = true;
                               wait_until(
                                   [&]
                                   {
                                       return release.load();
                                   });
                               running = false;
                           });
        CallbackArea second(1, 0,
                            [&]
                            {
                                if (running)
                                    overlap = true;
                                secondRan = true;
                            });

        workers[0].set_polling_wait(false);
        workers[1].set_polling_wait(false);
        workers[0].start(&executor, 0);
        workers[1].start(&executor, 1);

        executor.push(&first);
        const bool started = wait_until(
            [&]
            {
                return entered.load();
            });

        const auto before = executor.partition_runs();
        const auto changesBefore = executor.partition_changes();
        const auto other = (owner.load() + 1) % 2;
        const auto busyBefore = workers[other].statistics().busyNs;

        executor.push(&second);
        workers[other].wake();
        const bool finishedAttempt = wait_until(
            [&]
            {
                return workers[other].statistics().busyNs > busyBefore;
            });
        const bool excluded = secondRan.load() == false && executor.partition_changes() == changesBefore;
        const bool noEmptyAdmission = executor.partition_runs() == before;

        release = true;
        const bool progressed = wait_until(
            [&]
            {
                return secondRan.load();
            });

        workers[0].stop();
        workers[1].stop();
        workers[0].join();
        workers[1].join();

        require(started && finishedAttempt && noEmptyAdmission && excluded && progressed && overlap == false);
    }

    void wake_parallel_batch()
    {
        AreaExecuter executor(2);
        Worker workers[2];
        std::atomic_bool otherRan{false}, completed{false}, parallelProgress{false}, once{false};
        std::vector<std::unique_ptr<CallbackArea>> neighbors;

        for (int i = 1; i <= 128; ++i)
        {
            neighbors.emplace_back(std::make_unique<CallbackArea>(i * 5, 0,
                                                                  [&]
                                                                  {
                                                                      otherRan = true;
                                                                  }));
        }

        CallbackArea caller(0, 0,
                            [&]
                            {
                                if (once.exchange(true))
                                    return;

                                for (auto& area : neighbors)
                                    executor.push(area.get());

                                parallelProgress = wait_until(
                                    [&]
                                    {
                                        return otherRan.load();
                                    });
                                completed = true;
                            });

        workers[0].set_polling_wait(false);
        workers[1].set_polling_wait(false);
        workers[0].start(&executor, 0);
        workers[1].start(&executor, 1);
        const bool parked = wait_until(
            [&]
            {
                return workers[0].statistics().waitCount && workers[1].statistics().waitCount;
            });

        executor.push(&caller);
        const bool finished = wait_until(
            [&]
            {
                return completed.load();
            },
            3000);

        workers[0].stop();
        workers[1].stop();
        workers[0].join();
        workers[1].join();
        require(parked && finished && parallelProgress);
    }
} // namespace

void test_runtime()
{
    const AreaSchedulingOptions defaults{};
    require(defaults.unlimited && defaults.adaptive == false);

    adaptive_admission();
    adaptive_partition_barrier(false);
    adaptive_partition_barrier(true);
    area_profile();
    owner_timer_submission();
    wake_signal();
    fixed_object_period();
    optional_worker_diagnostics();
    wait_return_diagnostics();
    fixed_area_period();
    timer_trace_storage();
    timer_resolution_boundary();

    const auto before = Task::statistics();

    timer_boundaries();
    interruptible_wait();
    retained_worker_wake();
    defer_later_timer_wake();
    unnotified_ticker();
    queue_and_wakeup(false);
    queue_and_wakeup(true);
    accepted_submission_during_stop();
    partition_exclusion();
    wake_parallel_batch();

    for (int i = 0; i < 20; ++i)
        stop_during_submission();
    {
        Worker unused;
        unused.add_task(new ProbeTask());
        unused.add_task(new ProbeTask(), 5000);
    }

    const auto after = Task::statistics();
    require(after.live == before.live && after.created - before.created == after.destroyed - before.destroyed);
}
