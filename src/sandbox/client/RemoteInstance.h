// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in instance in a helper, as Phazite holds it (docs/tracktion-migration.md, T3): the channel Phazite made for its
// blocks, and the helper's instance on it, opened from the plug-in's format and file, and closed as it goes.
#pragma once

#include "Helper.h"
#include "sandbox/core/BlockChannel.h"

#include <memory>

namespace phazite::sandbox
{
class RemoteInstance final
{
public:
    // Which plug-in: its format's name, its file (or the format's identifier for it) and its name among the file's.
    struct Plugin
    {
        juce::String format, file, name;
    };

    // Opened in the helper at the sample rate for the channel's limits; nullptr, with why, if it wasn't.
    static std::unique_ptr<RemoteInstance> open (Helper&, const Plugin&, double sampleRate, const BlockChannel::Limits&,
                                                 juce::String& error, int timeoutMilliseconds);
    ~RemoteInstance();

    BlockChannel& getChannel() noexcept { return *channel; }
    int getLatencySamples() const noexcept { return latency; }

    RemoteInstance (const RemoteInstance&) = delete;
    RemoteInstance& operator= (const RemoteInstance&) = delete;

private:
    RemoteInstance (Helper&, std::int64_t id, std::unique_ptr<BlockChannel>);

    Helper& helper;
    const std::int64_t id;
    std::unique_ptr<BlockChannel> channel;
    int latency = 0;
};
} // namespace phazite::sandbox
