// SPDX-License-Identifier: AGPL-3.0-or-later
// What the pictures of the effects that sweep share (the phaser's, the flanger's and the chorus's, devices/motion/): their
// response over the spectrum of what they play (lit) and of what comes in (faint), drawn where their LFO has swept each side
// now, and under it a strip of the LFO, one cycle of its sweep with a dot for each side's copy (the right a part of a cycle
// ahead) and its rate, as Ableton's Phaser-Flanger shows its LFO, so that the sweep moves with the sound and a knob's turn
// shows at once.
// The device's own picture draws its response and its handles; a strip between the two may show more of it.
#pragma once

#include "DeviceGraph.h"
#include "devices/core/DeviceInfo.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/DisplayTicker.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace phazite::ui
{
class SweepPicture : public juce::Component,
                     private DisplayTicker::Display
{
public:
    SweepPicture (const DeviceHandle&, const devices::DeviceInfo&, int stripHeight = 0);
    ~SweepPicture() override;

    // Where the response is drawn; the device's strip under it (empty if it has none); and the LFO's strip at the bottom.
    juce::Rectangle<float> getResponseArea() const;
    juce::Rectangle<float> getStripArea() const;
    juce::Rectangle<float> getLfoArea() const;
    graph::FrequencyAxis getFrequencies() const { return { getResponseArea() }; }

    // Where the LFO has a side now, from -1 (the bottom of the sweep) to 1 (the top): from the phase the device last played
    // at, and the shape and stereo offset set now, so that a turn of those shows even while nothing plays.
    float getLfoValue (int channel) const;

    // How many copies each side sweeps, a dot each on the LFO's strip (the chorus's Ensemble has three; one at first); where
    // a side's copy is in the LFO's cycle now (whole cycles in or not: the right side a part of a cycle ahead); and the
    // sweep at a point in its cycle, from -1 to 1 (the LFO's shape at first).
    virtual int getNumVoices() const { return 1; }
    virtual double getVoicePhase (int channel, int voice) const;
    virtual float getSweepValue (double phase) const;

    void paint (juce::Graphics&) override;

    static constexpr int lfoHeight = 30;

protected:
    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the device's order
    std::vector<float> values;               // their values now, for its response

    double getSampleRate() const;
    void readValues();
    double getLfoPhase() const noexcept { return (double) phase; } // as the device last played, from 0 to 1

    // A parameter's index, by ID.
    int indexOf (std::string_view parameterID) const;

    // The device's response and handles over the spectrum (and its strip); and its LFO's phase as it plays (0 to 1).
    virtual void paintResponse (juce::Graphics&, const Palette&, juce::Colour tint) = 0;
    virtual float readPhase() const = 0;

private:
    const devices::DeviceInfo& info;
    const int stripHeight;
    const int shapeIndex, stereoIndex, syncIndex, divisionIndex, rateIndex;
    float phase = 0.0f;
    graph::SpectrumTrace input, output;
    DrawnNumbers drawn;

    void paintLfo (juce::Graphics&, const Palette&, juce::Colour tint) const;
    juce::String getRateText() const;
    void tick() override;
};
} // namespace phazite::ui
