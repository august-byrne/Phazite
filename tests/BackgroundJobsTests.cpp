// SPDX-License-Identifier: AGPL-3.0-or-later
// Jobs in the background on threads of their own, never ended by force (src/model/base/BackgroundJobs.h).
#include "TestHelpers.h"
#include "model/base/BackgroundJobs.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace
{
using phazite::BackgroundJobs;
using namespace std::chrono_literals;

void waitFor (const std::atomic<bool>& flag)
{
    while (! flag.load())
        std::this_thread::sleep_for (1ms);
}
} // namespace

TEST_CASE ("Background jobs run in the order they come, one at a time on one thread")
{
    BackgroundJobs jobs ("Phazite test jobs", 1, juce::Thread::Priority::normal);
    std::mutex lock;
    std::vector<int> order;

    for (int i = 0; i < 20; ++i)
        jobs.add (
            [&lock, &order, i] (const BackgroundJobs::ShouldStop&)
            {
                const std::scoped_lock held (lock);
                order.push_back (i);
            });

    jobs.waitForAll();
    CHECK (jobs.getNumJobs() == 0);

    std::vector<int> expected (20);
    std::iota (expected.begin(), expected.end(), 0);
    const std::scoped_lock held (lock);
    CHECK (order == expected);
}

TEST_CASE ("Background jobs run on no more threads at once than they may")
{
    BackgroundJobs jobs ("Phazite test jobs", 2, juce::Thread::Priority::normal);
    std::atomic<int> running { 0 }, most { 0 }, done { 0 };

    for (int i = 0; i < 6; ++i)
        jobs.add (
            [&] (const BackgroundJobs::ShouldStop&)
            {
                const auto now = ++running;

                for (auto seen = most.load(); now > seen && ! most.compare_exchange_weak (seen, now);)
                {
                }

                std::this_thread::sleep_for (20ms);
                --running;
                ++done;
            });

    jobs.waitForAll();
    CHECK (done == 6);
    CHECK (most.load() >= 1);
    CHECK (most.load() <= 2);
}

TEST_CASE ("Removing background jobs drops those that wait, and waits for those that run, asking them to stop if so")
{
    BackgroundJobs jobs ("Phazite test jobs", 1, juce::Thread::Priority::normal);

    SUBCASE ("Asked to stop, the one running stops; the one waiting never runs")
    {
        std::atomic<bool> started { false }, stopped { false }, laterRan { false };

        jobs.add (
            [&] (const BackgroundJobs::ShouldStop& shouldStop)
            {
                started = true;

                while (! shouldStop())
                    std::this_thread::sleep_for (1ms);

                stopped = true;
            });
        jobs.add ([&] (const BackgroundJobs::ShouldStop&) { laterRan = true; });

        waitFor (started);
        CHECK (jobs.getNumJobs() == 2);
        jobs.removeAll (true);
        CHECK (stopped);
        CHECK (jobs.getNumJobs() == 0);
        jobs.waitForAll();
        CHECK_FALSE (laterRan);
    }

    SUBCASE ("Not asked to stop, the one running finishes; the one waiting never runs")
    {
        std::atomic<bool> started { false }, finished { false }, laterRan { false };

        jobs.add (
            [&] (const BackgroundJobs::ShouldStop& shouldStop)
            {
                started = true;
                std::this_thread::sleep_for (50ms);
                finished = ! shouldStop();
            });
        jobs.add ([&] (const BackgroundJobs::ShouldStop&) { laterRan = true; });

        waitFor (started);
        jobs.removeAll (false);
        CHECK (finished);
        CHECK (jobs.getNumJobs() == 0);
        jobs.waitForAll();
        CHECK_FALSE (laterRan);
    }

    // Jobs added afterwards run as before.
    std::atomic<bool> ran { false };
    jobs.add ([&ran] (const BackgroundJobs::ShouldStop&) { ran = true; });
    jobs.waitForAll();
    CHECK (ran);
}

TEST_CASE ("A background job far slower to stop than JUCE's thread pool allows is waited for as its jobs go, never ended by force")
{
    // juce::ThreadPool ends a thread that hasn't stopped within half a second, which can leave a lock it held held for
    // ever; this job takes longer than that and never looks at whether it should stop.
    std::atomic<bool> started { false }, finished { false };

    {
        BackgroundJobs jobs ("Phazite test jobs", 1, juce::Thread::Priority::normal);
        jobs.add (
            [&] (const BackgroundJobs::ShouldStop&)
            {
                started = true;
                std::this_thread::sleep_for (800ms);
                finished = true;
            });

        waitFor (started);
    }

    CHECK (finished);
}
