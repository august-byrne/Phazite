// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper as Phazite's engine plays it: each block straight through the channel of its instance
// (SandboxedInstance.h), one round trip with a deadline of the block's own length in real time (a millisecond at least),
// so that a plug-in that hangs makes its own blocks late and never the song's. A block the helper hasn't answered in time,
// one a render holds the channel for, one that holds what isn't a number or comes back otherwise than it went, and every
// block of a plug-in whose helper has gone or whose channel waits to be prepared are silent: an effect's sound fades out
// over a few milliseconds (its input, as late as the plug-in would have played it), an instrument's stops; and the
// plug-in's sound fades in again as it plays again. A block kept for seconds is a hang, which its helper is ended for
// (sandbox/client/Helper.h). A render waits for the plug-in instead (for a block of live playback it is still on too), for
// as long as it takes, as REAPER's and Bitwig's do (a plug-in may only be slow: known bug 42, when a render ended a helper
// that kept a block for 30 seconds), a little at a time: until its helper has gone, which the render then says
// (hasFailedInRender()), as it does for a plug-in that had crashed before it began (OfflineRender.h), or until the render
// is cancelled. Once it has waited as long as a hang takes live, the render's window names the plug-in it waits for
// (OfflineRender::findWait()), so that the user can cancel it. The notes of a block that couldn't be handed over go with the next that
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
    bool playsChannelsApart() const noexcept override { return true; } // each message on its channel

    // An instrument is handed its block as the engine's block starts, so that the plug-in instruments of every track play
    // side by side in their helpers (docs/archive/performance-audit.md, section 10).
    bool startsEarly() const noexcept override { return info.kind == devices::DeviceKind::instrument; }
    void start (const devices::ProcessContext&) override;

    // For the render that plays it (any thread): whether a block of the render's wasn't played because the plug-in's helper
    // went (a crash, or ended for a hang), since this was made; the render cancelled, which ends its wait for a block; and
    // since when a render waits for its block (sandbox::getNanoseconds()), or 0 while none does.
    bool hasFailedInRender() const noexcept { return failedInRender.load (std::memory_order_acquire); }
    void cancelRender() noexcept { renderCancelled.store (true, std::memory_order_release); }
    std::int64_t getRenderWaitSince() const noexcept { return renderWaitSince.load (std::memory_order_acquire); }

    // How long its sound takes to fade out as it stops playing (an effect's then silent rather than dry: a limiter or a
    // fully wet reverb passed dry is worse than a gap), and to fade in as it plays again.
    static constexpr double fadeSeconds = 0.005;

private:
    std::shared_ptr<SandboxedInstance> kept;
    SandboxedInstance& instance;
    std::string typeID, name;
    std::vector<std::string> ids, names; // which the parameters' descriptions point into
    std::vector<devices::ParameterInfo> parameters;
    devices::DeviceInfo info;
    double rate = 44100.0;
    int maximumBlock = 512;
    std::int64_t busySince = 0;          // the audio thread's: when it found the channel still on a block, in high-resolution ticks
    bool resetWanted = false;            // the audio thread's: reset() since a block was last handed over
    int handedChannels = 0;              // the audio thread's: the channels of the block handed over last, which it reads back
    std::uint16_t channelsWithNotes = 0; // the audio thread's: the MIDI channels it has given the plug-in notes on, a bit each

    // The audio thread's: how much of the plug-in's sound is heard (0 while it plays nothing, rising and falling over the
    // fade), and an effect's input over the last of its latency and a block, from which it fades out as late as the plug-in
    // would have played it (made as it is prepared, for the latency it says then, as the engine's delays of other sounds
    // are).
    float heard = 1.0f;
    float fadeStep = 1.0f;
    bool startsSilent = false; // until it is first prepared: it loaded while silence played in its place
    int latency = 0, historyAt = 0;
    juce::AudioBuffer<float> history;

    // The audio thread's: a block started early (start()), whether it was handed over, and when, in high-resolution ticks.
    bool started = false, handed = false;
    std::int64_t handedAt = 0;

    // The audio thread's: the events of blocks that couldn't be handed over, for the start of the next that is, their room
    // made as this is. A note that began and ended among them isn't played.
    std::vector<devices::NoteEvent> waiting;

    std::atomic<bool> failedInRender { false }, renderCancelled { false };
    std::atomic<std::int64_t> renderWaitSince { 0 };

    bool handOver (const devices::ProcessContext&) noexcept;                    // held and handed over, or silent
    void collect (const devices::ProcessContext&, std::int64_t since) noexcept; // its answer, then the channel let go
    bool waitInRender() noexcept;                                               // for a render's block: whether it was answered
    void skip (const devices::ProcessContext&) noexcept;                        // a block not handed over: its events kept
    void keep (std::span<const devices::NoteEvent>) noexcept;
    void remember (const devices::ProcessContext&) noexcept; // an effect's input, before it is handed over
    void silence (const devices::ProcessContext&) noexcept;  // what it plays in place of a block it didn't play
    void fadeIn (const devices::ProcessContext&) noexcept;   // a block it played, after one it didn't
};

// What the engine plays in a plug-in's place while it still loads, as its project opened (PluginInstances.h): silence, an
// effect's included, as for a late or crashed plug-in (an effect that can't play is silent, never dry: a limiter passed dry
// is worse than a gap), so that the strip it is on is silent until it has loaded; then the plug-in's own processor fades
// its sound in (SandboxedInstance::joinLate()). An instrument or an effect, as its description says.
class LoadingPlugin final : public devices::Processor
{
public:
    explicit LoadingPlugin (bool isInstrument);

    const devices::DeviceInfo& getInfo() const noexcept { return info; }

    void prepare (double, int) override {}
    void reset() override {}
    void process (const devices::ProcessContext&) override;
    bool sleepsOnSilence() const noexcept override { return false; }

private:
    devices::DeviceInfo info;
};
} // namespace phazite
