// SPDX-License-Identifier: AGPL-3.0-or-later
// The loudness meter at the end of the master's devices (docs/design.md, section 8): Tracktion's real-time-safe
// EBU R128 meter (te::LoudnessMeter) as a plug-in, so that it runs in the playback graph. It measures what
// the master's devices put out, before the master volume.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class LoudnessMeterPlugin : public te::Plugin
{
public:
    explicit LoudnessMeterPlugin (te::PluginCreationInfo);
    ~LoudnessMeterPlugin() override;

    static const char* xmlTypeName;
    static juce::ValueTree create();

    // Momentary, short-term and integrated loudness in LUFS, and the true peak. Lock-free, any thread.
    te::LoudnessMeter::Readings getReadings() const noexcept { return meter.getReadings(); }

    // Starts the integrated loudness and the held peaks again.
    void resetReadings() noexcept { meter.requestReset(); }

    juce::String getName() const override { return "Loudness Meter"; }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getShortName (int) override { return "LUFS"; }
    juce::String getSelectableDescription() override { return getName(); }
    bool canBeDisabled() override { return false; }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return numInputChannels; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override; // real-time safe: no locks, no allocation

private:
    static constexpr int maxChannels = 8;
    te::LoudnessMeter meter;

    // After a short-term window of silence its readings have fallen as far as they go, and it stops measuring until
    // sound comes again (docs/performance-audit.md, A1): the 4x-oversampled true peak costs most of a stopped master.
    static constexpr double sleepAfterSeconds = 3.5;
    int silentSamples = 0, sleepAfterSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterPlugin)
};

// The master's loudness meter, if it has one.
LoudnessMeterPlugin* findLoudnessMeter (const te::Edit&);

// Puts a loudness meter last among the master's devices, unless there is one. Projects call this when they
// are created or opened; it adds an undo step, which they clear.
LoudnessMeterPlugin* ensureLoudnessMeter (te::Edit&);
} // namespace phazite
