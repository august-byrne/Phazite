// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BackgroundJobs.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // The jobs a thread runs, on each of their own threads (isOnOwnThread()).
    thread_local const BackgroundJobs* jobsOfThisThread = nullptr;
} // namespace

// A thread that runs the jobs one after another, and sleeps while none waits.
class BackgroundJobs::Worker final : public juce::Thread
{
public:
    explicit Worker (BackgroundJobs& o)
        : juce::Thread (o.name),
          owner (o)
    {
        startThread (owner.priority);
    }

    // Woken and waited for, however long it takes to start or to end its job: never ended by force (the header says why).
    ~Worker() override { stopThread (-1); }

    void run() override
    {
        jobsOfThisThread = &owner;

        while (! threadShouldExit())
            if (! owner.runNext (*this))
                wait (-1); // until add() or stopThread() wakes it: a wake that comes first isn't lost
    }

private:
    BackgroundJobs& owner;
};

BackgroundJobs::BackgroundJobs (const juce::String& threadName, int maximumThreads, juce::Thread::Priority threadPriority)
    : name (threadName),
      maximum (std::max (1, maximumThreads)),
      priority (threadPriority)
{
}

BackgroundJobs::~BackgroundJobs()
{
    removeAll (true);
    const std::scoped_lock held (threadsLock);
    threads.clear();
}

void BackgroundJobs::add (Job job)
{
    {
        const std::scoped_lock held (lock);
        waiting.push_back (std::move (job));
    }

    const std::scoped_lock held (threadsLock);

    if ((int) threads.size() < maximum)
        threads.push_back (std::make_unique<Worker> (*this));

    for (const auto& thread : threads)
        thread->notify();
}

int BackgroundJobs::getNumJobs() const
{
    const std::scoped_lock held (lock);
    return (int) (waiting.size() + running.size());
}

void BackgroundJobs::waitForAll() const
{
    jassert (! isOnOwnThread()); // a job that waits for all of them waits for itself

    std::unique_lock held (lock);
    finished.wait (held, [this] { return waiting.empty() && running.empty(); });
}

bool BackgroundJobs::isOnOwnThread() const
{
    return jobsOfThisThread == this;
}

void BackgroundJobs::removeAll (bool stopRunning)
{
    jassert (! isOnOwnThread()); // a job that waits for those that run waits for itself

    std::deque<Job> dropped; // what they hold goes once the lock is let go

    std::unique_lock held (lock);
    dropped.swap (waiting);

    if (stopRunning)
        for (auto* job : running)
            job->stop = true;

    finished.wait (held, [this] { return running.empty(); });
}

void BackgroundJobs::dropWaiting()
{
    std::deque<Job> dropped; // what they hold goes once the lock is let go

    const std::scoped_lock held (lock);
    dropped.swap (waiting);
}

bool BackgroundJobs::runNext (const juce::Thread& thread)
{
    Job job;
    Running state;

    {
        const std::scoped_lock held (lock);

        if (waiting.empty())
            return false;

        job = std::move (waiting.front());
        waiting.pop_front();
        running.push_back (&state);
    }

    job ([&state, &thread] { return state.stop.load() || thread.threadShouldExit(); });
    job = nullptr; // what it holds goes before it counts as ended

    {
        const std::scoped_lock held (lock);
        std::erase (running, &state);
    }

    finished.notify_all();
    return true;
}
} // namespace phazite
