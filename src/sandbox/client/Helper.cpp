// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Helper.h"

#include <algorithm>

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
            if (! owner.process.isRunning())
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

    if (process.isRunning() && ! process.waitForProcessToFinish (quitMilliseconds))
        process.kill();

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

    if (! process.start (juce::StringArray { program.getFullPathName(), juce::String ("--") + helperProcessID + ":" + pipeName }, 0))
        return false;

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
            lost = false; // from now on a loss is told

            if (running)
                return true;
        }
    }

    running = false;
    lost = true;
    process.kill();
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
    process.kill();
    stopWaiting();
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
