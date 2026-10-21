// SPDX-License-Identifier: AGPL-3.0-or-later
// Displays that Phazite's synths share (docs/design.md, section 9): an envelope's shape, with how far the newest note has
// got; an LFO's wave, with where it is in its cycle; and a filter's response, which moves with the note as its envelope and
// LFO open and close it, with a dot at the cutoff (as high as the resonance) to drag. Their editors give them their
// parameters' handles and what the synth plays (functions read once a refresh); each refreshes from the DeviceTicker.
#pragma once

#include "DeviceGraph.h"
#include "DeviceTicker.h"
#include "model/devices/ParameterHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace phazite::ui
{
// An envelope's attack, decay, sustain and release, and a line at the level of the newest note's.
class EnvelopeView : public juce::Component,
                     private DeviceTicker::Display
{
public:
    // Times in milliseconds, the sustain from 0 to 1; level: the newest note's envelope now, from 0 to 1.
    EnvelopeView (const juce::String& title, ParameterHandle attack, ParameterHandle decay, ParameterHandle sustain,
                  ParameterHandle release, std::function<float()> level);

    float getShownLevel() const noexcept { return shownLevel; }
    void paint (juce::Graphics&) override;

private:
    ParameterHandle attack, decay, sustain, release;
    std::function<float()> level;
    float shownLevel = 0.0f;

    void tick() override;
};

// An LFO's wave, one cycle, and a dot where it is now.
class LfoView : public juce::Component,
                private DeviceTicker::Display
{
public:
    // shapeAt: the wave at a phase from 0 to 1, from -1 to 1; phase: where it is now, from 0 to 1 (below 0 for nowhere).
    LfoView (const juce::String& title, std::function<float (float phase)> shapeAt, std::function<float()> phase);

    void paint (juce::Graphics&) override;

private:
    std::function<float (float)> shapeAt;
    std::function<float()> phase;
    float shownPhase = -1.0f;

    void tick() override;
};

// An LFO's wave as the synths' LFO views draw it, by its shape's number (in dsp::LfoShape's order): dsp::Lfo's own
// values, and the random one as steps.
float getDrawnLfoValue (int shape, float phase);

// A filter's response: at its cutoff as set, and, fainter, where the newest note plays it now; a dot at the cutoff, as high
// as the resonance, which a drag moves (both, as one undo step).
class FilterView : public juce::Component,
                   private DeviceTicker::Display
{
public:
    // readValues: the synth's parameters' values, read once for each drawing; magnitude: the response for them, as a gain, with
    // the cutoff at a frequency, at a frequency; playing: the cutoff the newest note plays now (0 for none).
    using Response = std::function<double (const std::vector<float>& values, double cutoff, double frequency)>;
    FilterView (const juce::String& title, ParameterHandle cutoff, ParameterHandle resonance,
                std::function<std::vector<float>()> readValues, Response magnitude, std::function<float()> playing);

    juce::Point<float> getDotPosition() const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    ParameterHandle cutoff, resonance;
    std::function<std::vector<float>()> readValues;
    Response magnitude;
    std::function<float()> playing;
    float shownPlaying = 0.0f;

    graph::FrequencyAxis getFrequencies() const;
    void dragTo (juce::Point<float>);
    void tick() override;
};
} // namespace phazite::ui
