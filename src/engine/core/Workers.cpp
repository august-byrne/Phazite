// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Workers.h"

#include <thread>

namespace phazite::engine
{
namespace
{
    constexpr int spins = 4000; // a moment's checks for the next job before a worker sleeps

#if JUCE_WINDOWS
    // MMCSS's "Pro Audio" task for the thread that makes it, while it lives, as JUCE's WASAPI thread joins it
    // (juce_WASAPI_windows.cpp, which loads avrt.dll the same way).
    class ProAudioTask
    {
    public:
        ProAudioTask()
        {
            if (auto* join = reinterpret_cast<Join> (avrt.getFunction ("AvSetMmThreadCharacteristicsW")))
            {
                unsigned long index = 0;
                task = join (L"Pro Audio", &index);
            }
        }

        ~ProAudioTask()
        {
            if (task != nullptr)
                if (auto* leave = reinterpret_cast<Leave> (avrt.getFunction ("AvRevertMmThreadCharacteristics")))
                    leave (task);
        }

    private:
        using Join = void*(__stdcall*) (const wchar_t*, unsigned long*);
        using Leave = int (__stdcall*) (void*);

        juce::DynamicLibrary avrt { "avrt.dll" };
        void* task = nullptr;

        JUCE_DECLARE_NON_COPYABLE (ProAudioTask)
    };
#endif
} // namespace

class Workers::Worker final : private juce::Thread
{
public:
    explicit Worker (Workers& w)
        : juce::Thread ("Phazite engine worker"),
          workers (w)
    {
#if JUCE_WINDOWS
        // Not JUCE's real-time thread, which on Windows puts the whole process in the real-time priority class (the high
        // one without an administrator's rights), every thread of the app above other programs'; the thread joins MMCSS's
        // Pro Audio task itself (run()).
        startThread (juce::Thread::Priority::highest);
#else
        startRealtimeThread (juce::Thread::RealtimeOptions {}.withPriority (10));
#endif
    }

    ~Worker() override
    {
        signalThreadShouldExit();
        stopThread (2000);
    }

private:
    Workers& workers;

    void run() override
    {
#if JUCE_WINDOWS
        const ProAudioTask proAudio;
#endif
        auto seen = workers.generation.load (std::memory_order_acquire);

        while (! threadShouldExit() && ! workers.quitting.load (std::memory_order_acquire))
        {
            // The next job: spun for a moment, then slept for.
            auto now = workers.generation.load (std::memory_order_acquire);

            for (int i = 0; i < spins && now == seen; ++i)
            {
                std::this_thread::yield();
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

void Workers::setNumThreads (int count)
{
    const auto wanted = (size_t) std::max (0, count - 1);

    if (wanted == threads.size())
        return;

    // All go and come anew: a sleeping one is woken to go.
    quitting.store (true, std::memory_order_release);
    generation.fetch_add (1, std::memory_order_acq_rel);
    generation.notify_all();
    threads.clear();
    quitting.store (false, std::memory_order_release);

    for (size_t i = 0; i < wanted; ++i)
        threads.push_back (std::make_unique<Worker> (*this));
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

    task.store (newTask, std::memory_order_relaxed);
    context.store (newContext, std::memory_order_relaxed);
    numTasks.store (count, std::memory_order_relaxed);
    doneTasks.store (0, std::memory_order_relaxed);
    next.store (((next.load (std::memory_order_relaxed) >> 32) + 1) << 32, std::memory_order_release);
    generation.fetch_add (1, std::memory_order_acq_rel);
    generation.notify_all();

    takeTasks();

    // The tasks others took, done soon.
    while (doneTasks.load (std::memory_order_acquire) < count)
        std::this_thread::yield();
}

void Workers::takeTasks() noexcept
{
    for (;;)
    {
        // The job read after which job it is: a task taken only if neither changed meanwhile.
        auto taken = next.load (std::memory_order_acquire);
        const auto count = numTasks.load (std::memory_order_acquire);
        const auto run = task.load (std::memory_order_acquire);
        auto* const with = context.load (std::memory_order_acquire);
        const auto index = (int) (taken & 0xffffffffu);

        if (index >= count)
            return;

        if (! next.compare_exchange_weak (taken, taken + 1, std::memory_order_acq_rel))
            continue;

        run (with, index);
        doneTasks.fetch_add (1, std::memory_order_acq_rel);
    }
}
} // namespace phazite::engine
