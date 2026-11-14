// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Helper.h"

#include <algorithm>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
#endif

// Built with a sanitizer (the linux-asan and linux-tsan presets): GCC says so by these, Clang by __has_feature.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
 #define PHAZITE_SANITIZED 1
#elif defined(__has_feature)
 #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
  #define PHAZITE_SANITIZED 1
 #endif
#endif

#ifndef PHAZITE_SANITIZED
 #define PHAZITE_SANITIZED 0
#endif

namespace phazite::sandbox
{
namespace
{
    // The pipe's reads and writes wait for as long as it takes: JUCE takes a read that times out for the pipe's loss, and
    // the pipe is quiet while a song plays. How long a helper asked to quit has before it is ended, and how often the
    // watching thread looks at the process.
    constexpr int pipeTimeoutMilliseconds = -1;
    constexpr int quitMilliseconds = 1000;
    constexpr int watchMilliseconds = 200;
    constexpr double heartbeatMilliseconds = 1000.0; // how often the message thread is asked whether it answers

    // The program's arguments: the pipe it connects to; and, built with a sanitizer on Linux and macOS, a copy of this
    // process's stderr, which it inherits (JUCE's ChildProcess keeps what isn't closed on exec) and writes its own to, so
    // that its reports (a race, a leak, undefined behaviour, a bad access) reach the tests' output as the tests' own do
    // (tools/ci-sanitizer-reports.sh). Nothing of Phazite's reads the helper's output: a reader would wait on a pipe that
    // a process the plug-in started could hold open after the helper went. Otherwise its output goes nowhere (JUCE's
    // ChildProcess sends what it isn't asked to read there). JUCE workaround W31 (docs/juce-workarounds.md).
    juce::StringArray getArguments (const juce::File& program, const juce::String& pipeName)
    {
        juce::StringArray arguments { program.getFullPathName(), juce::String ("--") + helperProcessID + ":" + pipeName };

#if PHAZITE_SANITIZED && ! JUCE_WINDOWS
        static const int errors = ::dup (STDERR_FILENO);

        if (errors >= 0)
            arguments.add (juce::String ("--") + helperErrorsID + ":" + juce::String (errors));
#endif

        return arguments;
    }

#if JUCE_WINDOWS
    // The job the helpers are in, which ends them as this process ends however it ends (a crash, or ended from outside),
    // as the system closes the job's last handle then: made once, its handle never closed nor inherited. What a helper
    // starts (a plug-in's browser for its manual, a licence manager) isn't put in it, and outlives Phazite as it would
    // have outlived a plug-in hosted in Phazite's own process.
    HANDLE getHelpersJob()
    {
        static const HANDLE job = []
        {
            auto* made = CreateJobObjectW (nullptr, nullptr);

            if (made != nullptr)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
                SetInformationJobObject (made, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
            }

            return made;
        }();

        return job;
    }
#endif

    // A helper put where it ends as Phazite does: on Windows, in the helpers' job; on Linux and macOS it watches for its
    // parent to go itself (host/core/Host.h). By the process ID the helper tells as it starts, as JUCE's ChildProcess
    // keeps its handle of the process to itself: JUCE workaround W32 (docs/juce-workarounds.md).
    void endWithThisProcess ([[maybe_unused]] std::int64_t processID)
    {
#if JUCE_WINDOWS
        if (auto* job = getHelpersJob(); job != nullptr)
        {
            if (auto* process = OpenProcess (PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, (DWORD) processID); process != nullptr)
            {
                AssignProcessToJobObject (job, process);
                CloseHandle (process);
            }
        }
#endif
    }
} // namespace

class Helper::Pipe final : public juce::InterprocessConnection
{
public:
    explicit Pipe (Helper& h)
        : juce::InterprocessConnection (false), // its callbacks on its own thread
          owner (h)
    {
    }

    ~Pipe() override { disconnect(); }

private:
    Helper& owner;

    void connectionMade() override {}
    void connectionLost() override { owner.helperLost(); }
    void messageReceived (const juce::MemoryBlock& message) override { owner.received (message); }
};

class Helper::Watch final : private juce::Thread
{
public:
    explicit Watch (Helper& h)
        : juce::Thread ("Phazite helper watch"),
          owner (h)
    {
        startThread (juce::Thread::Priority::low);
    }

    // Waited for, never ended by force, which can leave a lock it held held for ever: the pipe is closed first, which
    // wakes a heartbeat it may be sending (closePipe()).
    ~Watch() override { stopThread (-1); }

private:
    Helper& owner;

    void run() override
    {
        auto beatSent = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            // Gone: what waits on the pipe is woken too, as a write to a helper that never connected would wait for ever.
            if (! owner.isProcessRunning())
            {
                owner.helperLost();
                owner.closePipe();
                return;
            }

            // Starting, and not answered in time: it may never have connected to the pipe, which a write waits for. Ended,
            // its loss wakes the write.
            if (const auto deadline = owner.startDeadline.load(); deadline > 0.0 && juce::Time::getMillisecondCounterHiRes() > deadline)
            {
                owner.kill();
            }
            else if (owner.running && looksHung (beatSent))
            {
                owner.endedForHanging = true;
                owner.kill(); // which the next look tells of
            }

            wait (watchMilliseconds);
        }
    }

    // An audio thread told to have kept a block too long; or the message thread not answering the heartbeat, sent a
    // second after it last answered, for as long as it may (its clock started again while a request waits for its own
    // answer, which the message thread may be busy with).
    bool looksHung (double& beatSent)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (owner.hung.exchange (false))
            return true;

        {
            const juce::ScopedLock held (owner.lock);

            if (! owner.waiting.empty())
            {
                beatSent = now;
                return false;
            }
        }

        if (owner.heartbeat.load() == 0)
        {
            if (now - beatSent >= heartbeatMilliseconds)
            {
                auto beat = owner.makeRequest (MessageType::heartbeat);
                owner.heartbeat = beat.getRequest();
                beatSent = now;
                owner.post (beat);
            }

            return false;
        }

        return now - beatSent > owner.messageLimit.load();
    }
};

Helper::Helper (const juce::File& programToStart)
    : program (programToStart)
{
}

Helper::~Helper()
{
    if (running)
    {
        MessageWriter quit (MessageType::quit, 0);
        post (quit);
    }

    running = false;
    lost = true; // no loss told as it goes
    stopWaiting();

    {
        const juce::ScopedLock held (processLock);

        if (process.isRunning() && ! process.waitForProcessToFinish (quitMilliseconds))
            process.kill();
    }

    closePipe();
    watch = nullptr;
    pipe = nullptr;
}

void Helper::closePipe()
{
    // What waits on the pipe woken (a read, or a write that waits for the helper to connect, as JUCE's pipe does for one
    // that never connected or has gone), and its thread ended; what is sent after it goes nowhere. The watching thread
    // and the helper's owner may both close it, one after the other.
    const juce::ScopedLock held (pipeLock);

    if (pipe != nullptr)
        pipe->disconnect();
}

bool Helper::start (int timeoutMilliseconds)
{
    if (! program.existsAsFile())
        return false;

    // A pipe of a name no other has, which the helper connects to.
    const auto pipeName = "phazite-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());
    pipe = std::make_unique<Pipe> (*this);

    if (! pipe->createPipe (pipeName, pipeTimeoutMilliseconds, true))
        return false;

    {
        const juce::ScopedLock held (processLock);

        if (! process.start (getArguments (program, pipeName), 0))
            return false;
    }

    // Until it has answered, a loss isn't told (a start that fails says so itself), and the watching thread ends a helper
    // that doesn't answer in time: one that never connects to the pipe would keep the hello's write waiting for ever.
    running = true;
    lost = true;
    startDeadline = juce::Time::getMillisecondCounterHiRes() + timeoutMilliseconds;
    watch = std::make_unique<Watch> (*this);
    auto hello = makeRequest (MessageType::hello);
    hello.add (protocolVersion);
    const auto answer = request (hello, timeoutMilliseconds);
    startDeadline = 0.0;

    if (answer.has_value())
    {
        MessageReader reader (*answer);

        if (reader.readInt() == protocolVersion)
        {
            processID = reader.readInt64();
            endWithThisProcess (processID);
            lost = false; // from now on a loss is told

            if (running)
                return true;
        }
    }

    running = false;
    lost = true;
    killProcess();
    closePipe();
    watch = nullptr;
    return false;
}

std::optional<juce::MemoryBlock> Helper::request (MessageWriter& message, int timeoutMilliseconds)
{
    if (! running)
        return {};

    Waiting wait;
    wait.request = message.getRequest();

    {
        const juce::ScopedLock held (lock);
        waiting.push_back (&wait);
    }

    const auto sent = pipe->sendMessage (message.take());
    const auto answered = sent && wait.answered.wait ((double) timeoutMilliseconds);

    {
        const juce::ScopedLock held (lock);
        std::erase (waiting, &wait);
    }

    if (! answered || ! running || wait.reply.isEmpty())
        return {};

    return std::move (wait.reply);
}

bool Helper::post (MessageWriter& message)
{
    return running && pipe->sendMessage (message.take());
}

void Helper::kill()
{
    // The process, whose end the watching thread then tells of.
    running = false;
    killProcess();
    stopWaiting();
}

bool Helper::isProcessRunning()
{
    const juce::ScopedLock held (processLock);
    return process.isRunning();
}

void Helper::killProcess()
{
    const juce::ScopedLock held (processLock);
    process.kill();
}

void Helper::setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds) noexcept
{
    messageLimit = messageThreadMilliseconds;
    audioLimit = audioThreadMilliseconds;
}

void Helper::received (const juce::MemoryBlock& message)
{
    MessageReader reader (message);

    if (reader.getType() == MessageType::reply)
    {
        // The heartbeat answered: the message thread is alive.
        if (auto beat = heartbeat.load(); beat != 0 && reader.getRequest() == beat)
        {
            heartbeat.compare_exchange_strong (beat, 0);
            return;
        }

        const juce::ScopedLock held (lock);

        for (auto* wait : waiting)
        {
            if (wait->request == reader.getRequest())
            {
                wait->reply = message; // read again whole with MessageReader
                wait->answered.signal();
                return;
            }
        }

        return; // one given up on
    }

    if (onNotice != nullptr)
        onNotice (reader);
}

void Helper::helperLost()
{
    running = false;
    stopWaiting();

    if (! lost.exchange (true) && onLost != nullptr)
        onLost();
}

void Helper::stopWaiting()
{
    const juce::ScopedLock held (lock);

    for (auto* wait : waiting)
        wait->answered.signal();
}
} // namespace phazite::sandbox
