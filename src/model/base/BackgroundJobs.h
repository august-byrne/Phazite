// SPDX-License-Identifier: AGPL-3.0-or-later
// Jobs run in the background, in the order they come, on up to so many threads of their own, started as jobs come: what
// juce::ThreadPool does, but never ending a thread by force. JUCE's pool ends a thread that hasn't stopped within half a
// second of being asked, and juce::Thread::stopThread() with a time limit one that hasn't stopped in time, with
// TerminateThread on Windows, which leaves whatever lock the thread held held for ever: the system's loader lock, held
// by a thread still starting, left every thread started after it waiting for ever, and the process hung (tools/
// check-architecture.sh counts such calls: thread-kills). Here a job is asked to stop and waited for, however long it
// takes; a job that may take long looks at whether it should stop between its steps.
// JUCE workaround W1 (docs/juce-workarounds.md).
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace phazite
{
class BackgroundJobs final
{
public:
    // A job: what it does, given what says whether it should stop (once removeAll() asks it to, or the jobs go).
    using ShouldStop = std::function<bool()>;
    using Job = std::function<void (const ShouldStop&)>;

    BackgroundJobs (const juce::String& threadName, int maximumThreads, juce::Thread::Priority);
    ~BackgroundJobs(); // removeAll (true), then its threads end, each waited for

    // A job to run once a thread is free: one started for it while there are fewer than the maximum. Any thread.
    void add (Job);

    // How many jobs wait or run.
    int getNumJobs() const;

    // Waits until no job waits or runs, however long that takes. Not on one of its own threads.
    void waitForAll() const;

    // Drops the jobs that wait, and waits for those that run, however long they take, asking them to stop first if
    // stopRunning. Not on one of its own threads.
    void removeAll (bool stopRunning);

    BackgroundJobs (const BackgroundJobs&) = delete;
    BackgroundJobs& operator= (const BackgroundJobs&) = delete;

private:
    class Worker;

    // A job running, which removeAll() may ask to stop.
    struct Running
    {
        std::atomic<bool> stop { false };
    };

    const juce::String name;
    const int maximum;
    const juce::Thread::Priority priority;

    mutable std::mutex lock;
    mutable std::condition_variable finished; // a running job has ended
    std::deque<Job> waiting;
    std::vector<Running*> running;

    std::mutex threadsLock; // held while a thread is added or they go
    std::vector<std::unique_ptr<Worker>> threads;

    // Runs the next job on a thread of its own; false if none waits.
    bool runNext (const juce::Thread&);
};
} // namespace phazite
