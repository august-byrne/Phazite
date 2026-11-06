// SPDX-License-Identifier: AGPL-3.0-or-later
// An on/off switch that looks as a track's mute switch does (TrackSwitch.h), in every place where something is switched on or
// off (the owner, 2026-11-06: "all ON switches that we have right now should be unified to the same green toggle item that is
// used in the track lanes"): a device, in the Devices view and the mixer; a modulator; a part of a device, as a synth's LFO or
// a wavetable's oscillator; an EQ's band. A small light, lit in the playing colour while on, a hollow ring while off, so that
// shape tells them apart as well as colour. A click switches it; screen readers read it as a switch, on or off.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class PowerSwitch : public juce::Button
{
public:
    // Named for screen readers and tooltips ("On", or what it switches: "LFO on").
    explicit PowerSwitch (const juce::String& name = "On");

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;
};
} // namespace phazite::ui
