// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper as Phazite's engine plays it (docs/tracktion-migration.md, T3): each block straight
// through the channel of its instance (SandboxedInstance.h), one round trip with a deadline of the block's own length in
// real time (none in a render), so that a plug-in that hangs makes its own blocks late and never the song's. A block the
// helper hasn't answered in time, one a render holds the channel for, and every block of a plug-in whose helper has gone
// let an effect's sound through, and are silent for an instrument. Its mix with what came in is the engine's, as for
// Phazite's own devices: the one parameter its description has.
#pragma once

#include "SandboxedInstance.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/base/internal/Tracktion.h"

#include <string>

namespace phazite
{
class SandboxedProcessor final : public devices::Processor
{
public:
    // The plug-in Tracktion wraps, whose instance is the one in a helper.
    SandboxedProcessor (te::Plugin&, SandboxedInstance&);

    const devices::DeviceInfo& getInfo() const noexcept { return info; }
    const te::Plugin& getPlugin() const noexcept { return *plugin; }
    const SandboxedInstance& getInstance() const noexcept { return instance; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override {}
    void process (const devices::ProcessContext&) override;
    int getLatencySamples() const override;
    double getTailSeconds() const override;
    bool sleepsOnSilence() const noexcept override { return false; }

private:
    te::Plugin::Ptr plugin; // which keeps the instance
    SandboxedInstance& instance;
    std::string typeID, name;
    devices::ParameterInfo mix;
    devices::DeviceInfo info;
    double rate = 44100.0;
};
} // namespace phazite
