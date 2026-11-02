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
} // namespace

RemoteInstance::RemoteInstance (Helper& h, std::int64_t instanceID, std::unique_ptr<BlockChannel> blockChannel)
    : helper (h),
      id (instanceID),
      channel (std::move (blockChannel))
{
}

std::unique_ptr<RemoteInstance> RemoteInstance::open (Helper& helper, const Plugin& plugin, double sampleRate,
                                                      const BlockChannel::Limits& limits, juce::String& error, int timeoutMilliseconds)
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
    message.add (instance->id).add (juce::String (name)).add (limits.maxSamples).add (limits.maxChannels).add (limits.maxSidechainChannels);
    message.add (limits.maxEvents).add (limits.maxChanges).add (sampleRate).add (plugin.format).add (plugin.file).add (plugin.name);
    const auto answer = helper.request (message, timeoutMilliseconds);

    if (! answer.has_value())
    {
        error = helper.isRunning() ? "The plug-in took too long to load." : "The plug-in's helper has gone.";
        return nullptr;
    }

    MessageReader reader (*answer);

    if (! reader.readBool())
    {
        error = reader.readString();
        return nullptr;
    }

    reader.readString();
    instance->latency = reader.readInt();
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
} // namespace phazite::sandbox
