// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Host.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <vector>

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
    editors.clear();   // before their plug-ins
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
        {
            const auto id = reader.readInt64();
            editors.erase (id);
            instances.erase (id);
            break;
        }

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

        case MessageType::parameterValue:
            if (auto* instance = findInstance (reader.readInt64()))
            {
                const auto index = reader.readInt();
                answer.add ((double) instance->getParameterValue (index, reader.readString()));
            }
            break;

        case MessageType::stats:
            answer.add (getProcessCpuSeconds()).add (getProcessMemoryBytes());
            break;

        case MessageType::setProgram:
            if (auto* instance = findInstance (reader.readInt64()))
                instance->setProgram (reader.readInt());
            break;

        case MessageType::openEditor:
            openEditor (reader.readInt64(), answer);
            break;

        case MessageType::closeEditor:
            editors.erase (reader.readInt64());
            break;

        case MessageType::scan:
            scan (reader, answer);
            break;

        // Notices: nothing to answer.
        case MessageType::setParameter:
            if (auto* instance = findInstance (reader.readInt64()))
            {
                const auto index = reader.readInt();
                instance->setParameter (index, (float) reader.readDouble());
            }
            return;

        case MessageType::placeEditor:
            placeEditor (reader);
            return;

        case MessageType::setEditorKeys:
            setEditorKeys (reader);
            return;

        case MessageType::quit:
            juce::JUCEApplicationBase::quit();
            return;

        case MessageType::parameterChanged:
        case MessageType::gesture:
        case MessageType::latencyChanged:
        case MessageType::stateChanged:
        case MessageType::parametersChanged:
        case MessageType::editorResized:
        case MessageType::editorKey:
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
        setPluginState (*plugin, state);

    auto instance = std::make_unique<Instance> (id, std::move (plugin), std::move (channel), sampleRate);
    answer.add (true).add (juce::String());
    sandbox::write (answer, instance->describe());
    instances[id] = std::move (instance);
}

void Host::openEditor (std::int64_t id, MessageWriter& answer)
{
    // The plug-in's own, made once (asked again, the one there is), not shown until Phazite places it.
    auto* instance = findInstance (id);
    auto& editor = editors[id];

    if (instance != nullptr && editor == nullptr)
    {
        const auto resized = [this, id] (int width, int height)
        {
            MessageWriter notice (MessageType::editorResized, 0);
            notice.add (id).add ((std::int32_t) width).add ((std::int32_t) height);
            send (notice);
        };

        const auto key = [this, id] (bool isDown, const juce::KeyPress& pressed)
        {
            MessageWriter notice (MessageType::editorKey, 0);
            notice.add (id)
                .add (isDown)
                .add ((std::int32_t) pressed.getKeyCode())
                .add ((std::int32_t) pressed.getModifiers().getRawFlags());
            send (notice);
        };

        editor = std::make_unique<Editor> (instance->getPlugin(), resized, key);
    }

    if (editor == nullptr || ! editor->isOpen())
    {
        editors.erase (id);
        answer.add (false);
        return;
    }

    answer.add (true).add ((std::int32_t) editor->getBounds().getWidth()).add ((std::int32_t) editor->getBounds().getHeight());
    answer.add (editor->isResizable());
}

void Host::scan (MessageReader& reader, MessageWriter& answer)
{
    // What the file holds, as its format finds it: a crash or a hang here is the helper's, which Phazite ends.
    const auto formatName = reader.readString();
    const auto file = reader.readString();
    juce::OwnedArray<juce::PluginDescription> found;

    for (auto* format : formats.getFormats())
        if (format->getName() == formatName)
            format->findAllTypesForFile (found, file);

    answer.add ((std::int32_t) found.size());

    for (const auto* description : found)
        answer.add (description->createXml()->toString (juce::XmlElement::TextFormat().withoutHeader().singleLine()));
}

void Host::placeEditor (MessageReader& reader)
{
    const auto id = reader.readInt64();
    const auto window = reader.readInt64();
    const auto x = reader.readInt();
    const auto y = reader.readInt();
    const auto width = reader.readInt();
    const auto height = reader.readInt();

    if (const auto found = editors.find (id); found != editors.end())
        found->second->place (window, { x, y, width, height });
}

void Host::setEditorKeys (MessageReader& reader)
{
    const auto id = reader.readInt64();
    const auto getsEveryKey = reader.readBool();
    std::vector<juce::KeyPress> keys ((size_t) std::max (0, reader.readInt()));

    for (auto& key : keys)
    {
        const auto code = reader.readInt();
        key = juce::KeyPress (code, juce::ModifierKeys (reader.readInt()), 0);
    }

    if (const auto found = editors.find (id); found != editors.end())
        found->second->setKeys (getsEveryKey, std::move (keys));
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
