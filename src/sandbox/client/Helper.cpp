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

    ~Watch() override { stopThread (2000); }

private:
    Helper& owner;

    void run() override
    {
        while (! threadShouldExit())
        {
            if (! owner.process.isRunning())
            {
                owner.helperLost();
                return;
            }

            wait (watchMilliseconds);
        }
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
    watch = nullptr;
    stopWaiting();

    if (process.isRunning() && ! process.waitForProcessToFinish (quitMilliseconds))
        process.kill();

    pipe = nullptr;
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

    running = true;
    watch = std::make_unique<Watch> (*this);
    auto hello = makeRequest (MessageType::hello);
    hello.add (protocolVersion);
    const auto answer = request (hello, timeoutMilliseconds);

    if (answer.has_value())
    {
        MessageReader reader (*answer);

        if (reader.readInt() == protocolVersion)
            return true;
    }

    running = false;
    watch = nullptr;
    process.kill();
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

void Helper::received (const juce::MemoryBlock& message)
{
    MessageReader reader (message);

    if (reader.getType() == MessageType::reply)
    {
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
