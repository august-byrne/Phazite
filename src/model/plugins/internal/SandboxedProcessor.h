// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper as Phazite's engine plays it (docs/tracktion-migration.md, T3): each block straight
// through the channel of its instance (SandboxedInstance.h), one round trip with a deadline of the block's own length in
// real time (none in a render), so that a plug-in that hangs makes its own blocks late and never the song's. A block the
// helper hasn't answered in time, one a render holds the channel for, and every block of a plug-in whose helper has gone
// let an effect's sound through, and are silent for an instrument; a block kept for seconds is a hang, which its helper
// is ended for (sandbox/client/Helper.h). Its mix with what came in is the engine's, as for
// Phazite's own devices. Its parameters are the plug-in's, by their places as Tracktion names them ("0", "1"...), so that
// the song's automation, modulators and macros move them on Phazite's engine as they move its own devices': the block
// goes whole, with their values on each control point as its changes (devices/core/Processor.h), which the helper plays
// at their samples.
#pragma once

#include "SandboxedInstance.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/base/internal/Tracktion.h"

#include <string>
#include <vector>

namespace phazite
{
class SandboxedProcessor final : public devices::Processor
{
public:
    // The plug-in Tracktion wraps, whose instance is the one in a helper.
    SandboxedProcessor (te::Plugin&, SandboxedInstance&);

    // What the engine knows of it: the plug-in's parameters, each from 0 to 1, then an effect's mix. It lives as long as
    // the processor.
    const devices::DeviceInfo& getInfo() const noexcept { return info; }
    const te::Plugin& getPlugin() const noexcept { return *plugin; }
    SandboxedInstance& getInstance() const noexcept { return instance; }

    // A parameter's index by Tracktion's ID of it, or -1 for one that isn't the plug-in's (its dry and wet levels).
    int findParameter (const juce::String& tracktionID) const noexcept;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override {}
    void process (const devices::ProcessContext&) override;
    int getLatencySamples() const override;
    double getTailSeconds() const override;
    bool sleepsOnSilence() const noexcept override { return false; }
    bool takesWholeBlocks() const noexcept override { return true; }

    // An instrument is handed its block as the engine's block starts, so that the plug-in instruments of every track play
    // side by side in their helpers (docs/performance-audit.md, section 10).
    bool startsEarly() const noexcept override { return info.kind == devices::DeviceKind::instrument; }
    void start (const devices::ProcessContext&) override;

private:
    te::Plugin::Ptr plugin; // which keeps the instance
    SandboxedInstance& instance;
    std::string typeID, name;
    std::vector<std::string> ids, names; // which the parameters' descriptions point into
    std::vector<devices::ParameterInfo> parameters;
    devices::DeviceInfo info;
    double rate = 44100.0;
    std::int64_t busySince = 0; // the audio thread's: when it found the channel still on a block, in high-resolution ticks

    // The audio thread's: a block started early (start()), whether it was handed over, and when, in high-resolution ticks.
    bool started = false, handed = false;
    std::int64_t handedAt = 0;

    bool handOver (const devices::ProcessContext&) noexcept;                    // held and handed over, or silent
    void collect (const devices::ProcessContext&, std::int64_t since) noexcept; // its answer, then the channel let go
    void silence (const devices::ProcessContext&) const noexcept;
};
} // namespace phazite
