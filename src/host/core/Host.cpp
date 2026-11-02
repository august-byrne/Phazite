// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Host.h"

#include <juce_events/juce_events.h>

namespace phazite::host
{
using sandbox::MessageReader;
using sandbox::MessageType;
using sandbox::MessageWriter;

class Host::Pipe final : public juce::InterprocessConnection
{
public:
    explicit Pipe (Host& h)
        : juce::InterprocessConnection (false), // its callbacks on its own thread
          owner (h)
    {
    }

    ~Pipe() override { disconnect(); }

private:
    Host& owner;

    void connectionMade() override {}

    // Phazite closed, or crashed: the helper goes too.
    void connectionLost() override
    {
        juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
    }

    void messageReceived (const juce::MemoryBlock& message) override { owner.received (message); }
};

Host::Host()
{
    addPluginFormats (formats);
}

Host::~Host()
{
    instances.clear(); // their threads stopped, before the pipe goes
    pipe = nullptr;
}

bool Host::connect (const juce::String& commandLine)
{
    const auto prefix = juce::String ("--") + sandbox::helperProcessID + ":";
    const auto argument = commandLine.fromFirstOccurrenceOf (prefix, false, false).upToFirstOccurrenceOf (" ", false, false).unquoted();

    if (argument.isEmpty())
        return false;

    // Its reads wait for as long as it takes (JUCE takes one that times out for the pipe's loss): the pipe is quiet while
    // a song plays, and breaks at once if Phazite goes.
    pipe = std::make_unique<Pipe> (*this);
    return pipe->connectToPipe (argument, -1);
}

void Host::received (const juce::MemoryBlock& message)
{
    // On the pipe's thread: handled on the message thread, in the order they came.
    juce::MessageManager::callAsync ([this, message] { handle (message); });
}

void Host::handle (const juce::MemoryBlock& message)
{
    MessageReader reader (message);
    MessageWriter answer (MessageType::reply, reader.getRequest());

    switch (reader.getType())
    {
        case MessageType::hello:
            reader.readInt();
            answer.add (sandbox::protocolVersion);
            break;

        case MessageType::heartbeat:
            break;

        case MessageType::openInstance:
            openInstance (reader, answer);
            break;

        case MessageType::closeInstance:
            instances.erase (reader.readInt64());
            break;

        case MessageType::stats:
            answer.add (getProcessCpuSeconds()).add (getProcessMemoryBytes());
            break;

        case MessageType::quit:
            juce::JUCEApplicationBase::quit();
            return;

        case MessageType::reply:
            return;
    }

    reply (answer);
}

void Host::reply (MessageWriter& answer)
{
    if (pipe != nullptr)
        pipe->sendMessage (answer.take());
}

void Host::openInstance (MessageReader& reader, MessageWriter& answer)
{
    const auto id = reader.readInt64();
    const auto channelName = reader.readString();
    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = reader.readInt();
    limits.maxChannels = reader.readInt();
    limits.maxSidechainChannels = reader.readInt();
    limits.maxEvents = reader.readInt();
    limits.maxChanges = reader.readInt();
    const auto sampleRate = reader.readDouble();
    const auto format = reader.readString();
    const auto file = reader.readString();
    const auto name = reader.readString();

    juce::String error;
    auto channel = sandbox::BlockChannel::open (channelName.toStdString(), limits);
    auto plugin = channel != nullptr ? makePlugin (format, file, name, sampleRate, limits.maxSamples, error) : nullptr;

    if (channel == nullptr)
        error = "The helper couldn't open the plug-in's channel.";

    if (plugin == nullptr)
    {
        answer.add (false).add (error.isNotEmpty() ? error : juce::String ("The plug-in didn't load.")).add (0);
        return;
    }

    auto instance = std::make_unique<Instance> (std::move (plugin), std::move (channel), sampleRate);
    const auto latency = instance->getPlugin().getLatencySamples();
    instances[id] = std::move (instance);
    answer.add (true).add (juce::String()).add (latency);
}

std::unique_ptr<juce::AudioPluginInstance> Host::makePlugin (const juce::String& formatName, const juce::String& file,
                                                             const juce::String& name, double sampleRate, int blockSize,
                                                             juce::String& error)
{
    for (auto* format : formats.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        // The type in the file by its name, or its first.
        juce::OwnedArray<juce::PluginDescription> types;
        format->findAllTypesForFile (types, file);

        for (const auto* type : types)
            if (name.isEmpty() || type->name == name)
                return format->createInstanceFromDescription (*type, sampleRate, blockSize, error);
    }

    error = "No plug-in " + name + " in " + file + ".";
    return nullptr;
}
} // namespace phazite::host
