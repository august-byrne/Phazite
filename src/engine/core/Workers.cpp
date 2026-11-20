// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Workers.h"
#include "devices/core/AudioThreads.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

namespace phazite::engine
{
namespace
{
    // How long a worker spins for the next job before it sleeps: long enough to keep up with blocks played back to back
    // (a render), not a device's next block, a few milliseconds away (spinning 4000 yields kept every worker busy between
    // blocks: three took half a core in an empty project).
    constexpr auto spinTime = std::chrono::microseconds (50);
} // namespace

class Workers::Worker final : private juce::Thread
{
public:
    Worker (Workers& w, Use u)
        : juce::Thread (u == Use::live ? "Phazite engine worker" : "Phazite render worker"),
          workers (w),
          use (u)
    {
        if (use == Use::live)
            devices::startAudioThread (*this); // at the audio device's priority, the process's left as it is
        else
            startThread (juce::Thread::Priority::normal); // a render's: as the thread that renders
    }

    ~Worker() override
    {
        // Woken by setNumThreads(), and waited for: never ended by force, which can leave a lock it held held for ever.
        signalThreadShouldExit();
        stopThread (-1);
    }

private:
    Workers& workers;
    const Use use;

    void run() override
    {
        // As the audio thread plays (Engine::process()): numbers too small to hear are zero, so that what dies away (a
        // container's crossover, a modulator's follower) doesn't slow down, and plays the same on any thread. A device's own
        // guard puts this back as it returns.
        const juce::ScopedNoDenormals noDenormals;

        // In the audio device's task while it plays its blocks; a render's is no audio thread (Workers.h).
        std::optional<devices::ProAudioTask> proAudio;

        if (use == Use::live)
            proAudio.emplace();

        auto seen = workers.generation.load (std::memory_order_acquire);

        while (! threadShouldExit() && ! workers.quitting.load (std::memory_order_acquire))
        {
            // The next job: spun for a moment, then slept for.
            auto now = workers.generation.load (std::memory_order_acquire);
            const auto spinUntil = std::chrono::steady_clock::now() + spinTime;

            while (now == seen && std::chrono::steady_clock::now() < spinUntil)
            {
                devices::pauseInSpin();
                now = workers.generation.load (std::memory_order_acquire);
            }

            if (now == seen)
            {
                workers.generation.wait (seen, std::memory_order_acquire);
                continue;
            }

            seen = now;
            workers.takeTasks();
        }
    }
};

Workers::Workers() = default;

Workers::~Workers()
{
    setNumThreads (1);
}

void Workers::setNumThreads (int count, Use newUse)
{
    const auto wanted = (size_t) std::max (0, count - 1);

    if (wanted == threads.size() && (newUse == use || threads.empty()))
    {
        use = newUse;
        return;
    }

    use = newUse;

    // All go and come anew: a sleeping one is woken to go.
    quitting.store (true, std::memory_order_release);
    generation.fetch_add (1, std::memory_order_acq_rel);
    generation.notify_all();
    threads.clear();
    quitting.store (false, std::memory_order_release);

    for (size_t i = 0; i < wanted; ++i)
        threads.push_back (std::make_unique<Worker> (*this, use));
}

void Workers::run (int count, Task newTask, void* newContext) noexcept
{
    if (count <= 0)
        return;

    // Alone, or for a single task: on the audio thread.
    if (threads.empty() || count == 1)
    {
        for (int i = 0; i < count; ++i)
            newTask (newContext, i);

        return;
    }

    jassert (count <= maximumTasks);
    count = std::min (count, maximumTasks);
    task.store (newTask, std::memory_order_relaxed);
    context.store (newContext, std::memory_order_relaxed);
    doneTasks.store (0, std::memory_order_relaxed);
    next.store ((((next.load (std::memory_order_relaxed) >> 32) + 1) << 32) | ((std::uint64_t) count << 16), std::memory_order_release);
    generation.fetch_add (1, std::memory_order_acq_rel);
    generation.notify_all();

    takeTasks();

    // The tasks others took, done soon: spun for, never giving the core away.
    devices::spinUntil ([this, count] { return doneTasks.load (std::memory_order_acquire) >= count; });
}

void Workers::takeTasks() noexcept
{
    for (;;)
    {
        // The job read after which job it is, with its count: a task taken only if neither changed meanwhile. (A job's task
        // and context change only after all its tasks are done, which they can't be while one is still to take.)
        auto taken = next.load (std::memory_order_acquire);
        const auto count = (int) ((taken >> 16) & 0xffffu);
        const auto run = task.load (std::memory_order_acquire);
        auto* const with = context.load (std::memory_order_acquire);
        const auto index = (int) (taken & 0xffffu);

        if (index >= count)
            return;

        if (! next.compare_exchange_weak (taken, taken + 1, std::memory_order_acq_rel))
            continue;

        run (with, index);
        doneTasks.fetch_add (1, std::memory_order_acq_rel);
    }
}
} // namespace phazite::engine
