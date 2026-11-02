// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Host.h"

#include <juce_events/juce_events.h>

namespace phazite::host
{
using sandbox::MessageReader;
using sandbox::MessageType;
using sandbox::MessageWriter;

namespace
{
    constexpr int noticesPerSecond = 30;
} // namespace

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

sandbox::BlockChannel::Limits readLimits (MessageReader& reader)
{
    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = reader.readInt();
    limits.maxChannels = reader.readInt();
    limits.maxSidechainChannels = reader.readInt();
    limits.maxEvents = reader.readInt();
    limits.maxChanges = reader.readInt();
    return limits;
}

Host::Host()
{
    addPluginFormats (formats);
}

Host::~Host()
{
    stopTimer();
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

    if (! pipe->connectToPipe (argument, -1))
        return false;

    startTimerHz (noticesPerSecond);
    return true;
}

void Host::received (const juce::MemoryBlock& message)
{
    // On the pipe's thread: handled on the message thread, in the order they came.
    juce::MessageManager::callAsync ([this, message] { handle (message); });
}

Instance* Host::findInstance (std::int64_t id) const
{
    const auto found = instances.find (id);
    return found != instances.end() ? found->second.get() : nullptr;
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

        case MessageType::prepareInstance:
            prepareInstance (reader, answer);
            break;

        case MessageType::getState:
            if (auto* instance = findInstance (reader.readInt64()))
                answer.add (instance->getState());
            break;

        case MessageType::setState:
            if (auto* instance = findInstance (reader.readInt64()))
                instance->setState (reader.readBlock());
            break;

        case MessageType::parameterText:
            if (auto* instance = findInstance (reader.readInt64()))
            {
                const auto index = reader.readInt();
                answer.add (instance->getParameterText (index, (float) reader.readDouble()));
            }
            break;

        case MessageType::stats:
            answer.add (getProcessCpuSeconds()).add (getProcessMemoryBytes());
            break;

        // Notices: nothing to answer.
        case MessageType::setParameter:
            if (auto* instance = findInstance (reader.readInt64()))
            {
                const auto index = reader.readInt();
                instance->setParameter (index, (float) reader.readDouble());
            }
            return;

        case MessageType::quit:
            juce::JUCEApplicationBase::quit();
            return;

        case MessageType::parameterChanged:
        case MessageType::gesture:
        case MessageType::latencyChanged:
        case MessageType::stateChanged:
        case MessageType::parametersChanged:
        case MessageType::reply:
            return;
    }

    send (answer);
}

void Host::send (MessageWriter& message)
{
    if (pipe != nullptr)
        pipe->sendMessage (message.take());
}

void Host::timerCallback()
{
    for (auto& [id, instance] : instances)
        instance->takeNotices ([this] (MessageWriter& notice) { send (notice); });
}

void Host::openInstance (MessageReader& reader, MessageWriter& answer)
{
    const auto id = reader.readInt64();
    const auto channelName = reader.readString();
    const auto limits = readLimits (reader);
    const auto sampleRate = reader.readDouble();
    const auto descriptionXml = reader.readString();
    const auto state = reader.readBlock();

    const auto refuse = [&answer] (const juce::String& why) { answer.add (false).add (why); };
    juce::PluginDescription description;

    if (const auto xml = juce::parseXML (descriptionXml); xml == nullptr || ! description.loadFromXml (*xml))
        return refuse ("The plug-in's description didn't come whole.");

    auto channel = sandbox::BlockChannel::open (channelName.toStdString(), limits);

    if (channel == nullptr)
        return refuse ("The helper couldn't open the plug-in's channel.");

    juce::String error;
    auto plugin = formats.createPluginInstance (description, sampleRate, limits.maxSamples, error);

    if (plugin == nullptr)
        return refuse (error.isNotEmpty() ? error : "The plug-in didn't load.");

    // Its settings as the song keeps them, before it plays.
    if (! state.isEmpty())
        plugin->setStateInformation (state.getData(), (int) state.getSize());

    auto instance = std::make_unique<Instance> (id, std::move (plugin), std::move (channel), sampleRate);
    answer.add (true).add (juce::String());
    sandbox::write (answer, instance->describe());
    instances[id] = std::move (instance);
}

void Host::prepareInstance (MessageReader& reader, MessageWriter& answer)
{
    auto* instance = findInstance (reader.readInt64());
    const auto channelName = reader.readString();
    const auto limits = readLimits (reader);
    const auto sampleRate = reader.readDouble();
    auto channel = instance != nullptr ? sandbox::BlockChannel::open (channelName.toStdString(), limits) : nullptr;
    const auto opened = channel != nullptr;

    if (opened)
        instance->prepare (std::move (channel), sampleRate);

    answer.add (opened);
}
} // namespace phazite::host
