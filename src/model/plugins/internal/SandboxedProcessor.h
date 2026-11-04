// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper as Phazite's engine plays it: each block straight through the channel of its instance
// (SandboxedInstance.h), one round trip with a deadline of the block's own length in real time (none in a render), so
// that a plug-in that hangs makes its own blocks late and never the song's. A block the helper hasn't answered in time,
// one a render holds the channel for, and every block of a plug-in whose helper has gone let an effect's sound through,
// and are silent for an instrument; a block kept for seconds is a hang, which its helper is ended for
// (sandbox/client/Helper.h). Its mix with what came in is the engine's, as for Phazite's own devices. Its parameters
// are the plug-in's, by its own IDs for them (a VST3's numbers), so that the song's automation, modulators and macros
// move them on Phazite's engine as they move its own devices': the block goes whole, with their values on each control
// point as its changes (devices/core/Processor.h), which the helper plays at their samples.
#pragma once

#include "SandboxedInstance.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"

#include <memory>
#include <string>
#include <vector>

namespace phazite
{
class SandboxedProcessor final : public devices::Processor
{
public:
    // A plug-in's instance in its helper, which it keeps while it lives.
    explicit SandboxedProcessor (std::shared_ptr<SandboxedInstance>);

    // What the engine knows of it: the plug-in's parameters, each from 0 to 1, then an effect's mix. It lives as long as
    // the processor.
    const devices::DeviceInfo& getInfo() const noexcept { return info; }
    SandboxedInstance& getInstance() const noexcept { return instance; }

    // A parameter's index by the plug-in's own ID of it, or -1 for one that isn't the plug-in's (its mix).
    int findParameter (const juce::String& ownID) const;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override { resetWanted = true; } // the helper resets the plug-in as it is handed its next block
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
    std::shared_ptr<SandboxedInstance> kept;
    SandboxedInstance& instance;
    std::string typeID, name;
    std::vector<std::string> ids, names; // which the parameters' descriptions point into
    std::vector<devices::ParameterInfo> parameters;
    devices::DeviceInfo info;
    double rate = 44100.0;
    std::int64_t busySince = 0; // the audio thread's: when it found the channel still on a block, in high-resolution ticks
    bool resetWanted = false;   // the audio thread's: reset() since a block was last handed over

    // The audio thread's: a block started early (start()), whether it was handed over, and when, in high-resolution ticks.
    bool started = false, handed = false;
    std::int64_t handedAt = 0;

    bool handOver (const devices::ProcessContext&) noexcept;                    // held and handed over, or silent
    void collect (const devices::ProcessContext&, std::int64_t since) noexcept; // its answer, then the channel let go
    void silence (const devices::ProcessContext&) const noexcept;
};
} // namespace phazite
