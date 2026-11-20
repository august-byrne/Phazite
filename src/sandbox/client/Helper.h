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

    // Whether its process has ended, asked of the system now (any thread but the pipe's), rather than as the watching
    // thread last found it: a helper that crashed a moment ago still counts as running until that thread's next look.
    bool hasProcessEnded() { return ! isProcessRunning(); }

    // The helper's process's ID, as it told it once started (0 before). On Windows it is then in a job of Phazite's that
    // ends it as Phazite ends, however that is (not what it starts); on Linux and macOS it watches for Phazite to go
    // itself (host/core/Host.h).
    std::int64_t getProcessID() const noexcept { return processID.load(); }

    // Whether a process ID is a child of this process's running a program: the only ID a helper's hello is taken for (0
    // stays its ID otherwise: it isn't put in the job, and watches for Phazite to go itself).
    static bool isOwnChild (std::int64_t told, const juce::File& started);

    // A request's message, under a number of its own, for its fields; then sent with request(), which waits for its
    // answer (the reply, read with MessageReader), or none if it didn't come within the time or the helper went.
    MessageWriter makeRequest (MessageType type) { return { type, makeNumber() }; }

    // A number no other request or instance of this helper's has.
    std::int64_t makeNumber() noexcept { return nextRequest++; }
    std::optional<juce::MemoryBlock> request (MessageWriter&, int timeoutMilliseconds);

    // A request's answer, which comes on the pipe's thread: done once it has come, or once none will (the helper went, or
    // the request couldn't be sent), with its reply then (empty for none). Its asker keeps it while it waits for it: one let
    // go of is given up on, and an answer that comes after goes nowhere.
    class Reply
    {
    public:
        bool isDone() const noexcept { return done.load (std::memory_order_acquire); }
        bool wait (int timeoutMilliseconds) { return answered.wait ((double) timeoutMilliseconds) && isDone(); }
        const juce::MemoryBlock& getReply() const noexcept { return reply; } // once done

        // From now on the helper is taken for hung if its message thread doesn't answer the heartbeat meanwhile (any thread):
        // what waited for something that may take long (a plug-in's editor made) without giving up on it.
        void stopHoldingOffHang() noexcept { holdsOffHang.store (false); }

    private:
        friend class Helper;
        std::int64_t request = 0;
        std::atomic<bool> holdsOffHang { true };
        juce::WaitableEvent answered { true }; // stays signalled once it is
        juce::MemoryBlock reply;
        std::atomic<bool> done { false };
    };

    // A request sent without waiting for its answer, from any thread but the pipe's: while its reply is kept and not done,
    // the helper isn't taken for hung for not answering the heartbeat, as for request(), unless it doesn't hold that off
    // (a channel for another rate, which the helper makes at once: known bug 166).
    std::shared_ptr<Reply> send (MessageWriter&, bool holdsOffHang = true);

    // A message that wants no answer.
    bool post (MessageWriter&);

    // Ends the helper at once, as a hang asks, and keeps it from starting if start() hasn't started it yet (from any thread
    // but the pipe's: one that quits may end it while another starts it).
    void kill();

    // How long a helper asked to quit, as this goes, has before it is ended (a second; longer in a test that holds a
    // helper's ending).
    static constexpr int quitMilliseconds = 1000;
    void setQuitMilliseconds (int milliseconds) noexcept { quitLimit = milliseconds; }

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

    const juce::File program;
    juce::ChildProcess process;
    std::atomic<std::int64_t> processID { 0 };
    std::unique_ptr<Pipe> pipe;
    std::unique_ptr<Watch> watch; // stopped before the process and the pipe go
    std::atomic<bool> running { false }, lost { false }, hung { false }, endedForHanging { false };
    bool stopped = false; // kill() called: it doesn't start (processLock held)
    std::atomic<int> quitLimit { quitMilliseconds };
    std::atomic<std::int64_t> nextRequest { 1 };
    std::atomic<int> messageLimit { messageHangMilliseconds }, audioLimit { audioHangMilliseconds };
    std::atomic<std::int64_t> heartbeat { 0 }; // the heartbeat's request not answered yet, or 0
    std::atomic<double> startDeadline { 0.0 }; // while it starts: when it must have answered (getMillisecondCounterHiRes())
    juce::CriticalSection lock, pipeLock;
    juce::CriticalSection sendLock;            // held while a message is written to the pipe, by whichever thread sends it
    std::vector<std::weak_ptr<Reply>> waiting; // the requests whose answers are still wanted (those let go of are pruned)

    bool isWaiting();                               // for an answer still wanted (lock held)
    void finish (Reply&, const juce::MemoryBlock&); // done, with the reply (lock held)

    // The process is looked at by the watching thread while others end it or wait for it, and JUCE's ChildProcess isn't safe
    // to share (finding it gone forgets it): every use of it holds this (linux-tsan found the race, 2026-11-11).
    juce::CriticalSection processLock;

    bool write (MessageWriter&); // into the pipe, whole, one thread at a time
    void received (const juce::MemoryBlock&);
    void helperLost();
    void closePipe(); // as the helper is lost, and before the watching thread is stopped
    void stopWaiting();
    bool isProcessRunning();
    void killProcess();
};
} // namespace phazite::sandbox
