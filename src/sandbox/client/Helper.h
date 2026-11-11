// SPDX-License-Identifier: AGPL-3.0-or-later
// A helper process as Phazite holds it: the program started with the name of a pipe of Phazite's (JUCE's
// InterprocessConnection), the requests sent to it, each answered within a time limit or not at all, and what it tells
// unasked. A thread of its own watches the process, a fifth of a second at a time (JUCE's pipe, on Windows, waits for
// another to connect when the helper goes, rather than saying so): a helper that crashes is lost then, and the pipe
// closed, which wakes a write waiting for it to connect. One that hangs is ended (kill()), so that nothing of Phazite's
// waits for it: by the watching thread, when it hasn't answered as it starts within the time start() gives it, or when its
// message thread hasn't answered the heartbeat it sends each second for a while, but while a request waits for its own
// answer (a plug-in loading, its state); or when an audio thread of its has kept a block too long, which the engine tells
// it without waiting (reportHung()). The pipe's thread delivers the answers and the notices, the watching thread the
// loss; requests may come from any other. JUCE workaround W5 (docs/juce-workarounds.md), for the crashes. Built from JUCE's
// ChildProcess and InterprocessConnection rather than its ChildProcessCoordinator, which can't end a helper that hangs:
// JUCE workaround W28 (docs/juce-workarounds.md).
#pragma once

#include "sandbox/core/Messages.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite::sandbox
{
class Helper final
{
public:
    explicit Helper (const juce::File& program);
    ~Helper(); // asks the helper to quit, and ends it if it doesn't soon

    // Starts the program and checks that it speaks this version; false if it didn't start or answer within the time.
    bool start (int timeoutMilliseconds);

    bool isRunning() const noexcept { return running.load(); }

    // A request's message, under a number of its own, for its fields; then sent with request(), which waits for its
    // answer (the reply, read with MessageReader), or none if it didn't come within the time or the helper went.
    MessageWriter makeRequest (MessageType type) { return { type, makeNumber() }; }

    // A number no other request or instance of this helper's has.
    std::int64_t makeNumber() noexcept { return nextRequest++; }
    std::optional<juce::MemoryBlock> request (MessageWriter&, int timeoutMilliseconds);

    // A message that wants no answer.
    bool post (MessageWriter&);

    // Ends the helper at once, as a hang asks. Not on the pipe's thread.
    void kill();

    // How long its message thread may go without answering the heartbeat, and an audio thread of its keep a block,
    // before it is taken for hung and ended (shorter in the tests); and a block kept that long, told from any thread, the
    // audio thread's included (it waits for nothing).
    static constexpr int messageHangMilliseconds = 10000, audioHangMilliseconds = 5000;
    void setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds) noexcept;
    int getAudioHangMilliseconds() const noexcept { return audioLimit.load (std::memory_order_relaxed); }
    void reportHung() noexcept { hung.store (true, std::memory_order_relaxed); }
    bool wasEndedForHanging() const noexcept { return endedForHanging.load(); }

    // What the helper tells unasked (on the pipe's thread), and its loss (a crash, or kill()), once (on the watching
    // thread): set before start().
    std::function<void (MessageReader&)> onNotice;
    std::function<void()> onLost;

    Helper (const Helper&) = delete;
    Helper& operator= (const Helper&) = delete;

private:
    class Pipe;
    class Watch;

    struct Waiting
    {
        std::int64_t request = 0;
        juce::WaitableEvent answered;
        juce::MemoryBlock reply;
    };

    const juce::File program;
    juce::ChildProcess process;
    std::unique_ptr<Pipe> pipe;
    std::unique_ptr<Watch> watch; // stopped before the process and the pipe go
    std::atomic<bool> running { false }, lost { false }, hung { false }, endedForHanging { false };
    std::atomic<std::int64_t> nextRequest { 1 };
    std::atomic<int> messageLimit { messageHangMilliseconds }, audioLimit { audioHangMilliseconds };
    std::atomic<std::int64_t> heartbeat { 0 }; // the heartbeat's request not answered yet, or 0
    std::atomic<double> startDeadline { 0.0 }; // while it starts: when it must have answered (getMillisecondCounterHiRes())
    juce::CriticalSection lock, pipeLock;
    std::vector<Waiting*> waiting;

    // The process is looked at by the watching thread while others end it or wait for it, and JUCE's ChildProcess isn't safe
    // to share (finding it gone forgets it): every use of it holds this (linux-tsan found the race, 2026-11-11).
    juce::CriticalSection processLock;

    void received (const juce::MemoryBlock&);
    void helperLost();
    void closePipe(); // as the helper is lost, and before the watching thread is stopped
    void stopWaiting();
    bool isProcessRunning();
    void killProcess();
};
} // namespace phazite::sandbox
