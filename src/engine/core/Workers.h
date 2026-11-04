// SPDX-License-Identifier: AGPL-3.0-or-later
// The threads that help Phazite's engine play a block: the audio thread hands them a job of tasks, numbered, which it
// and they take in their order, each the next not taken yet, until all are done. A task waits for what it needs itself
// (its strip's inputs, done in tasks before it). The workers spin a moment for the next job, then sleep until it comes;
// the audio thread neither waits on a lock nor allocates, and wakes them without one (std::atomic's wait and notify).
// The workers play at the audio thread's priority, never raising the whole process's: on Windows in MMCSS's Pro Audio
// task, as JUCE's WASAPI thread does; elsewhere as real-time threads.
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace phazite::engine
{
class Workers
{
public:
    using Task = void (*) (void* context, int index) noexcept;

    Workers();
    ~Workers();

    // Message thread, while the audio thread runs no job (the engine is stopped, or prepared anew): so many threads in all,
    // the audio thread one of them (1 for none to help it).
    void setNumThreads (int);
    int getNumThreads() const noexcept { return (int) threads.size() + 1; }

    // Audio thread: runs a task for each index from 0 to numTasks - 1, on itself and the workers, and returns once all are
    // done. Real-time safe.
    void run (int numTasks, Task, void* context) noexcept;

private:
    class Worker;
    std::vector<std::unique_ptr<Worker>> threads;

    // The job, which the audio thread sets before it starts it; and which job it is, how many tasks it has and the next
    // not taken yet, in one value (the job's number in the top 32 bits, then the count, then the index, 16 bits each), so
    // that a thread takes a task of the job it read the count of, or none: read apart, a worker late from the last job
    // could read the next job's count with the last's index, and take a task twice. The generation wakes the workers.
    static constexpr int maximumTasks = 0xffff;
    std::atomic<Task> task { nullptr };
    std::atomic<void*> context { nullptr };
    std::atomic<int> doneTasks { 0 };
    std::atomic<std::uint64_t> next { 0 };
    std::atomic<std::uint32_t> generation { 0 };
    std::atomic<bool> quitting { false };
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free);

    void takeTasks() noexcept;
};
} // namespace phazite::engine
