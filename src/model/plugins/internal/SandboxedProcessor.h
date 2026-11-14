// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper as Phazite's engine plays it: each block straight through the channel of its instance
// (SandboxedInstance.h), one round trip with a deadline of the block's own length in real time, so that a plug-in that
// hangs makes its own blocks late and never the song's. A block the helper hasn't answered in time, one a render holds
// the channel for, one that holds what isn't a number, and every block of a plug-in whose helper has gone let an effect's
// sound through, and are silent for an instrument; a block kept for seconds is a hang, which its helper is ended for
// (sandbox/client/Helper.h). A render waits for the plug-in instead, but not for ever: until its helper has gone, until it
// has kept the block so long that it hangs (several times as long as a hang is live), or until the render is cancelled;
// the render then says so (hasFailedInRender()). The notes of a block that couldn't be handed over go with the next that
// is, at its start, so that no note that ended sounds on. Its mix with what came in is the engine's, as for Phazite's own
// devices. Its parameters are the plug-in's, by its own IDs for them (a VST3's numbers), so that the song's automation,
// modulators and macros move them on Phazite's engine as they move its own devices': the block goes whole, with their
// values on each control point as its changes (devices/core/Processor.h), which the helper plays at their samples.
#pragma once

#include "SandboxedInstance.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace phazite
{
class SandboxedProcessor final : public devices::Processor
{
public:
    // A plug-in's instance in its helper, which it keeps while it lives.
    explicit SandboxedProcessor (std::shared_ptr<SandboxedInstance>);

    // What the engine knows of it: an instrument or an effect as the plug-in's description says, which the song files it by
    // (whatever buses it has: an instrument may take sound in too); the plug-in's parameters, each from 0 to 1, then an
    // effect's mix. It lives as long as the processor.
    const devices::DeviceInfo& getInfo() const noexcept { return info; }
    SandboxedInstance& getInstance() const noexcept { return instance; }

    // A parameter's index by the plug-in's own ID of it, or -1 for one that isn't the plug-in's (its mix).
    int findParameter (const juce::String& ownID) const;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override; // the helper resets the plug-in as it is handed its next block
    void process (const devices::ProcessContext&) override;
    int getLatencySamples() const override;
    double getTailSeconds() const override;
    bool sleepsOnSilence() const noexcept override { return false; }
    bool takesWholeBlocks() const noexcept override { return true; }

    // An instrument is handed its block as the engine's block starts, so that the plug-in instruments of every track play
    // side by side in their helpers (docs/performance-audit.md, section 10).
    bool startsEarly() const noexcept override { return info.kind == devices::DeviceKind::instrument; }
    void start (const devices::ProcessContext&) override;

    // For the render that plays it (any thread): whether a block of the render's wasn't played because the plug-in's helper
    // went, or was ended for keeping the block too long, since this was made; and the render cancelled, which ends its
    // wait for a block.
    bool hasFailedInRender() const noexcept { return failedInRender.load (std::memory_order_acquire); }
    void cancelRender() noexcept { renderCancelled.store (true, std::memory_order_release); }

    // How many times as long as a hang is live (sandbox::Helper's audio hang limit) a render waits for a block.
    static constexpr int renderHangFactor = 6;

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

    // The audio thread's: the events of blocks that couldn't be handed over, for the start of the next that is, their room
    // made as this is. A note that began and ended among them isn't played.
    std::vector<devices::NoteEvent> waiting;

    const bool wasCrashed; // its helper had gone as this was made: nothing a render waits for
    std::atomic<bool> failedInRender { false }, renderCancelled { false };

    bool handOver (const devices::ProcessContext&) noexcept;                    // held and handed over, or silent
    void collect (const devices::ProcessContext&, std::int64_t since) noexcept; // its answer, then the channel let go
    bool waitInRender (sandbox::BlockChannel&) noexcept;                        // for a render's block: whether it was answered
    void skip (const devices::ProcessContext&) noexcept;                        // a block not handed over: its events kept
    void keep (std::span<const devices::NoteEvent>) noexcept;
    void silence (const devices::ProcessContext&) const noexcept;
};
} // namespace phazite
