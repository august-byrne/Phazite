// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in as Phazite's engine plays it (docs/tracktion-migration.md, T2 step 7): the instance Tracktion loaded, which
// Tracktion keeps loading, with its state and its window, played as a device through Tracktion's own call
// (te::Plugin::applyToBufferWithAutomation()). That call holds the plug-in's lock while it plays, so that a reload waits
// for the block; gives it its notes as MIDI, the song's place for its playhead, its mix (its dry and wet levels) and its
// automation (Tracktion's curves, which mirror the song's). A plug-in never sleeps on silence. T3's sandboxed host takes its
// place behind the same interface (devices/core/Processor.h).
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/base/internal/Tracktion.h"

#include <string>

namespace phazite
{
class PluginProcessor final : public devices::Processor
{
public:
    explicit PluginProcessor (te::Plugin&);
    ~PluginProcessor() override;

    // What the engine knows of it: an instrument or an effect, with no parameters of its own (Tracktion plays its
    // automation). It lives as long as the processor.
    const devices::DeviceInfo& getInfo() const noexcept { return info; }
    const te::Plugin& getPlugin() const noexcept { return *plugin; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override {}
    void process (const devices::ProcessContext&) override;
    int getLatencySamples() const override;
    double getTailSeconds() const override;
    bool sleepsOnSilence() const noexcept override { return false; }

private:
    te::Plugin::Ptr plugin;
    te::SafeSelectable<te::Edit> edit; // a plug-in whose Edit has gone is torn down no more
    std::string typeID, name;
    devices::DeviceInfo info;
    double sampleRate = 44100.0;
    bool initialised = false;
    static constexpr int maximumMidiMessages = 1024; // in a block; more are dropped
    te::MidiMessageArray midi;                       // its notes for a block, its room made as it is prepared
};
} // namespace phazite
