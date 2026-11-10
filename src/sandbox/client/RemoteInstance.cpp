// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RemoteInstance.h"

namespace phazite::sandbox
{
namespace
{
    // A channel's name no other has: short, for the operating systems' limits.
    std::string makeChannelName()
    {
        return "c" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()).substring (0, 14).toStdString();
    }

    void write (MessageWriter& message, const std::string& name, const BlockChannel::Limits& limits, double sampleRate)
    {
        message.add (juce::String (name)).add (limits.maxSamples).add (limits.maxChannels).add (limits.maxSidechainChannels);
        message.add (limits.maxEvents).add (limits.maxChanges).add (sampleRate);
    }
} // namespace

RemoteInstance::RemoteInstance (Helper& h, std::int64_t instanceID, std::unique_ptr<BlockChannel> blockChannel)
    : helper (h),
      id (instanceID),
      channel (std::move (blockChannel))
{
}

std::unique_ptr<RemoteInstance> RemoteInstance::open (Helper& helper, const juce::String& descriptionXml, const juce::MemoryBlock& state,
                                                      double sampleRate, const BlockChannel::Limits& limits, juce::String& error,
                                                      int timeoutMilliseconds)
{
    const auto name = makeChannelName();
    auto channel = BlockChannel::create (name, limits);

    if (channel == nullptr)
    {
        error = "Phazite couldn't make the plug-in's channel.";
        return nullptr;
    }

    std::unique_ptr<RemoteInstance> instance (new RemoteInstance (helper, helper.makeNumber(), std::move (channel)));
    auto message = helper.makeRequest (MessageType::openInstance);
    message.add (instance->id);
    write (message, name, limits, sampleRate);
    message.add (descriptionXml).add (state);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
    {
        error = helper.isRunning() ? "The plug-in took too long to load." : "The plug-in's helper has gone.";
        return nullptr;
    }

    MessageReader reader (*answer);
    const auto opened = reader.readBool();
    const auto why = reader.readString();

    if (! opened)
    {
        error = why;
        return nullptr;
    }

    instance->info = readPluginInfo (reader);
    instance->latency = instance->info.latencySamples;
    instance->program = instance->info.program;
    return instance;
}

RemoteInstance::~RemoteInstance()
{
    // The helper's instance goes before the channel it plays.
    if (helper.isRunning())
    {
        auto message = helper.makeRequest (MessageType::closeInstance);
        message.add (id);
        helper.request (message, 2000);
    }
}

bool RemoteInstance::prepare (double sampleRate, const BlockChannel::Limits& limits, int timeoutMilliseconds)
{
    const auto name = makeChannelName();
    auto newChannel = BlockChannel::create (name, limits);

    if (newChannel == nullptr)
        return false;

    auto message = helper.makeRequest (MessageType::prepareInstance);
    message.add (id);
    write (message, name, limits, sampleRate);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
        return false;

    MessageReader reader (*answer);

    if (! reader.readBool())
        return false;

    channel = std::move (newChannel);
    return true;
}

std::optional<juce::MemoryBlock> RemoteInstance::getState (int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::getState);
    message.add (id);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
        return {};

    MessageReader reader (*answer);
    return reader.readBlock();
}

bool RemoteInstance::setState (const juce::MemoryBlock& state, int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::setState);
    message.add (id).add (state);
    return helper.request (message, timeoutMilliseconds).has_value();
}

bool RemoteInstance::setProgram (int index, int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::setProgram);
    message.add (id).add ((std::int32_t) index);

    if (! helper.request (message, timeoutMilliseconds).has_value())
        return false;

    program = index;
    return true;
}

std::optional<RemoteInstance::EditorSize> RemoteInstance::openEditor (int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::openEditor);
    message.add (id);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
        return {};

    MessageReader reader (*answer);

    if (! reader.readBool())
        return {};

    EditorSize size;
    size.width = reader.readInt();
    size.height = reader.readInt();
    size.isResizable = reader.readBool();
    return size;
}

void RemoteInstance::placeEditor (std::int64_t window, int x, int y, int width, int height)
{
    MessageWriter message (MessageType::placeEditor, 0);
    message.add (id).add (window).add ((std::int32_t) x).add ((std::int32_t) y).add ((std::int32_t) width).add ((std::int32_t) height);
    helper.post (message);
}

void RemoteInstance::closeEditor (int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::closeEditor);
    message.add (id);
    helper.request (message, timeoutMilliseconds);
}

void RemoteInstance::setEditorKeys (bool getsEveryKey, const std::vector<std::pair<int, int>>& keys)
{
    MessageWriter message (MessageType::setEditorKeys, 0);
    message.add (id).add (getsEveryKey).add ((std::int32_t) keys.size());

    for (const auto& [code, modifiers] : keys)
        message.add ((std::int32_t) code).add ((std::int32_t) modifiers);

    helper.post (message);
}

void RemoteInstance::setParameter (int index, float value)
{
    MessageWriter message (MessageType::setParameter, 0);
    message.add (id).add ((std::int32_t) index).add ((double) value);
    helper.post (message);
}

std::optional<juce::String> RemoteInstance::getParameterText (int index, float value, int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::parameterText);
    message.add (id).add ((std::int32_t) index).add ((double) value);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
        return {};

    MessageReader reader (*answer);
    return reader.readString();
}

std::optional<float> RemoteInstance::getParameterValue (int index, const juce::String& text, int timeoutMilliseconds)
{
    auto message = helper.makeRequest (MessageType::parameterValue);
    message.add (id).add ((std::int32_t) index).add (text);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
        return {};

    MessageReader reader (*answer);
    return (float) reader.readDouble();
}

void RemoteInstance::handleNotice (MessageType type, MessageReader& notice)
{
    auto* told = listener.load();

    // Its latency and program, kept whoever listens.
    if (type == MessageType::latencyChanged && told == nullptr)
        latency = notice.readInt();

    if (type == MessageType::stateChanged && told == nullptr)
        program = notice.readInt();

    if (told == nullptr)
        return;

    switch (type)
    {
        case MessageType::parameterChanged:
        {
            const auto index = notice.readInt();
            told->parameterChanged (index, (float) notice.readDouble());
            break;
        }

        case MessageType::gesture:
        {
            const auto index = notice.readInt();
            told->gestureChanged (index, notice.readBool());
            break;
        }

        case MessageType::latencyChanged:
        {
            const auto samples = notice.readInt();
            latency = samples;
            told->latencyChanged (samples);
            break;
        }

        case MessageType::stateChanged:
            program = notice.readInt();
            told->stateChanged();
            break;

        case MessageType::parametersChanged:
            told->parametersChanged (readParameters (notice));
            break;

        case MessageType::editorResized:
        {
            const auto width = notice.readInt();
            told->editorResized (width, notice.readInt());
            break;
        }

        case MessageType::editorKey:
        {
            const auto isDown = notice.readBool();
            const auto code = notice.readInt();
            told->editorKey (isDown, code, notice.readInt());
            break;
        }

        case MessageType::hello:
        case MessageType::heartbeat:
        case MessageType::openInstance:
        case MessageType::closeInstance:
        case MessageType::prepareInstance:
        case MessageType::getState:
        case MessageType::setState:
        case MessageType::parameterText:
        case MessageType::parameterValue:
        case MessageType::stats:
        case MessageType::setProgram:
        case MessageType::openEditor:
        case MessageType::closeEditor:
        case MessageType::scan:
        case MessageType::quit:
        case MessageType::setParameter:
        case MessageType::placeEditor:
        case MessageType::setEditorKeys:
        case MessageType::reply:
            break;
    }
}
} // namespace phazite::sandbox
