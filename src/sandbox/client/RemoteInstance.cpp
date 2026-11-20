// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RemoteInstance.h"

#include <algorithm>

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

    // Answered within the time, as Helper::request() takes an answer: none if the helper went first.
    bool isAnswered (Helper::Reply& reply, int timeoutMilliseconds)
    {
        return reply.wait (timeoutMilliseconds) && ! reply.getReply().isEmpty();
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
    instance->channel->forgetName(); // opened by the helper, or never to be

    if (! answer.has_value())
    {
        error = helper.isRunning() ? "The plug-in took too long to load." : "The plug-in's helper has gone.";
        return nullptr;
    }

    MessageReader reader (*answer);
    const auto opened = reader.readBool();
    const auto why = readText (reader);

    if (! opened)
    {
        error = why;
        return nullptr;
    }

    instance->info = readPluginInfo (reader);
    instance->channelRate = sampleRate;
    instance->latency = instance->info.latencySamples;
    instance->program = instance->info.program;
    return instance;
}

RemoteInstance::~RemoteInstance()
{
    // The helper's instance closed, without waiting for the helper (known bug 69: closing a project waited for each plug-in
    // in turn): the helper keeps the channel's memory it opened until it has closed the instance, whatever this side does
    // with its own.
    if (helper.isRunning())
    {
        auto message = helper.makeRequest (MessageType::closeInstance);
        message.add (id);
        helper.post (message);
    }
}

bool RemoteInstance::prepare (double sampleRate, const BlockChannel::Limits& limits, int timeoutMilliseconds)
{
    auto asked = askToPrepare (sampleRate, limits);

    if (asked == nullptr)
        return false;

    asked->reply->wait (timeoutMilliseconds);
    return finishPreparing (*asked);
}

std::unique_ptr<RemoteInstance::Preparing> RemoteInstance::askToPrepare (double sampleRate, const BlockChannel::Limits& limits)
{
    const auto name = makeChannelName();
    auto newChannel = BlockChannel::create (name, limits);

    if (newChannel == nullptr)
        return nullptr;

    auto message = helper.makeRequest (MessageType::prepareInstance);
    message.add (id);
    write (message, name, limits, sampleRate);
    auto asked = std::make_unique<Preparing>();
    asked->channel = std::move (newChannel);
    asked->sampleRate = sampleRate;
    asked->reply = helper.send (message, false); // the heartbeat's own limit goes on meanwhile
    return asked;
}

bool RemoteInstance::finishPreparing (Preparing& asked)
{
    // Opened by the helper, or never to be: the helper keeps its name no longer.
    asked.channel->forgetName();

    if (! asked.reply->isDone() || asked.reply->getReply().isEmpty() || ! helper.isRunning())
        return false;

    MessageReader reader (asked.reply->getReply());

    if (! reader.readBool())
        return false;

    channel = std::move (asked.channel);
    channelRate = asked.sampleRate;
    return true;
}

std::optional<juce::MemoryBlock> RemoteInstance::getState (int timeoutMilliseconds)
{
    const auto reply = askState();
    reply->wait (timeoutMilliseconds);
    return readState (*reply);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askState()
{
    auto message = helper.makeRequest (MessageType::getState);
    message.add (id);
    return helper.send (message);
}

std::optional<juce::MemoryBlock> RemoteInstance::readState (const Helper::Reply& reply)
{
    if (! reply.isDone() || reply.getReply().isEmpty())
        return {};

    MessageReader reader (reply.getReply());
    return reader.readBlock();
}

bool RemoteInstance::setState (const juce::MemoryBlock& state, int timeoutMilliseconds)
{
    return isAnswered (*askToSetState (state), timeoutMilliseconds);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askToSetState (const juce::MemoryBlock& state)
{
    auto message = helper.makeRequest (MessageType::setState);
    message.add (id).add (state);
    return helper.send (message);
}

bool RemoteInstance::setProgram (int index, int timeoutMilliseconds)
{
    return isAnswered (*askToSetProgram (index), timeoutMilliseconds);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askToSetProgram (int index)
{
    // Its program from now on, as the helper sets it in turn.
    auto message = helper.makeRequest (MessageType::setProgram);
    message.add (id).add ((std::int32_t) index);
    program = index;
    return helper.send (message);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askToOpenEditor()
{
    auto message = helper.makeRequest (MessageType::openEditor);
    message.add (id);
    return helper.send (message);
}

std::optional<RemoteInstance::EditorSize> RemoteInstance::readEditor (const Helper::Reply& reply)
{
    if (! reply.isDone() || reply.getReply().isEmpty())
        return {};

    MessageReader reader (reply.getReply());

    if (! reader.readBool())
        return {};

    EditorSize size;
    size.width = readEditorSide (reader);
    size.height = readEditorSide (reader);
    size.isResizable = reader.readBool();
    return size;
}

void RemoteInstance::placeEditor (std::int64_t window, int x, int y, int width, int height)
{
    MessageWriter message (MessageType::placeEditor, 0);
    message.add (id).add (window).add ((std::int32_t) x).add ((std::int32_t) y).add ((std::int32_t) width).add ((std::int32_t) height);
    helper.post (message);
}

void RemoteInstance::closeEditor()
{
    auto message = helper.makeRequest (MessageType::closeEditor);
    message.add (id);
    helper.post (message);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askToCloseEditor()
{
    // Its answer comes once the helper has closed it; it holds off no hang (the helper closes it at once, and a window of
    // Phazite's that waits for it gives up in time).
    auto message = helper.makeRequest (MessageType::closeEditor);
    message.add (id);
    return helper.send (message, false);
}

void RemoteInstance::setEditorKeys (bool getsEveryKey, const std::vector<std::pair<int, int>>& keys,
                                    const std::vector<std::pair<int, int>>& places)
{
    MessageWriter message (MessageType::setEditorKeys, 0);
    message.add (id).add (getsEveryKey).add ((std::int32_t) keys.size());

    for (const auto& [code, modifiers] : keys)
        message.add ((std::int32_t) code).add ((std::int32_t) modifiers);

    message.add ((std::int32_t) places.size());

    for (const auto& [scanCode, modifiers] : places)
        message.add ((std::int32_t) scanCode).add ((std::int32_t) modifiers);

    helper.post (message);
}

void RemoteInstance::playLive()
{
    MessageWriter message (MessageType::playLive, 0);
    message.add (id);
    helper.post (message);
}

void RemoteInstance::setParameter (int index, float value)
{
    MessageWriter message (MessageType::setParameter, 0);
    message.add (id).add ((std::int32_t) index).add ((double) value);
    helper.post (message);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askParameterText (int index, float value)
{
    // Answered at once by a helper whose message thread answers: one that doesn't is ended for it, as for its heartbeat.
    auto message = helper.makeRequest (MessageType::parameterText);
    message.add (id).add ((std::int32_t) index).add ((double) value);
    return helper.send (message, false);
}

std::shared_ptr<Helper::Reply> RemoteInstance::askParameterValue (int index, const juce::String& text)
{
    auto message = helper.makeRequest (MessageType::parameterValue);
    message.add (id).add ((std::int32_t) index).add (keepText (text));
    return helper.send (message, false);
}

std::optional<juce::String> RemoteInstance::readParameterText (const Helper::Reply& reply)
{
    if (! reply.isDone() || reply.getReply().isEmpty())
        return {};

    MessageReader reader (reply.getReply());
    return readText (reader);
}

std::optional<float> RemoteInstance::readParameterValue (const Helper::Reply& reply)
{
    if (! reply.isDone() || reply.getReply().isEmpty())
        return {};

    MessageReader reader (reply.getReply());
    return readUnitValue (reader);
}

std::optional<juce::String> RemoteInstance::getParameterText (int index, float value, int timeoutMilliseconds)
{
    const auto reply = askParameterText (index, value);
    reply->wait (timeoutMilliseconds);
    return readParameterText (*reply);
}

std::optional<float> RemoteInstance::getParameterValue (int index, const juce::String& text, int timeoutMilliseconds)
{
    const auto reply = askParameterValue (index, text);
    reply->wait (timeoutMilliseconds);
    return readParameterValue (*reply);
}

void RemoteInstance::handleNotice (MessageType type, MessageReader& notice)
{
    auto* told = listener.load();

    // Its latency and program, kept whoever listens.
    if (type == MessageType::latencyChanged && told == nullptr)
        latency = keepLatency (notice.readInt());

    if (type == MessageType::stateChanged && told == nullptr)
        program = std::max (0, (int) notice.readInt());

    if (told == nullptr)
        return;

    switch (type)
    {
        case MessageType::parameterChanged:
        {
            const auto index = notice.readInt();

            if (const auto value = readUnitValue (notice))
                told->parameterChanged (index, *value);

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
            const auto samples = keepLatency (notice.readInt());
            latency = samples;
            told->latencyChanged (samples);
            break;
        }

        case MessageType::stateChanged:
            program = std::max (0, (int) notice.readInt());
            told->stateChanged();
            break;

        case MessageType::parametersChanged:
            told->parametersChanged (readParameters (notice));
            break;

        case MessageType::editorResized:
        {
            const auto width = readEditorSide (notice);
            told->editorResized (width, readEditorSide (notice));
            break;
        }

        case MessageType::editorKey:
        {
            const auto isDown = notice.readBool();
            const auto code = notice.readInt();
            const auto modifiers = keepModifiers (notice.readInt());
            told->editorKey (isDown, code, modifiers, juce::jlimit (0, 0xff, (int) notice.readInt()));
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
        case MessageType::playLive:
        case MessageType::reply:
            break;
    }
}
} // namespace phazite::sandbox
