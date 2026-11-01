// SPDX-License-Identifier: AGPL-3.0-or-later
// Displays that Phazite's synths share (docs/design.md, section 9): an envelope's shape, with how far the newest note has
// got; an LFO's wave, with where it is in its cycle; and a filter's response, which moves with the note as its envelope and
// LFO open and close it, with a dot at the cutoff (as high as the resonance) to drag. Their editors give them their
// parameters' handles and what the synth plays (functions read once a refresh); each refreshes from the DisplayTicker.
#pragma once

#include "DeviceGraph.h"
#include "model/devices/ParameterHandle.h"
#include "ui/components/DisplayTicker.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace phazite::ui
{
// An envelope's attack, decay, sustain and release, and a line at the level of the newest note's. Its points drag (the owner,
// 2026-11-01: a picture's key points should move as its knobs do, as Ableton's envelopes' breakpoints do): the peak sideways
// for the attack, the corner where the decay ends sideways for the decay and up and down for the sustain, and the end
// sideways for the release, each drag one undo step, Shift in fine steps; a double-click puts a point back where it
// started. While a point is dragged the picture keeps its scale, so that the point stays under the pointer. Its stages
// have no curve of their own to bend (they curve as analog envelopes do).
class EnvelopeView : public juce::Component,
                     private DisplayTicker::Display
{
public:
    // Times in milliseconds, the sustain from 0 to 1; level: the newest note's envelope now, from 0 to 1.
    EnvelopeView (const juce::String& title, ParameterHandle attack, ParameterHandle decay, ParameterHandle sustain,
                  ParameterHandle release, std::function<float()> level);

    float getShownLevel() const noexcept { return shownLevel; }

    enum class Point
    {
        attack, // the peak
        decay,  // the corner where the decay ends, at the sustain level
        release // the end
    };

    juce::Point<float> getPointPosition (Point) const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    ParameterHandle attack, decay, sustain, release;
    std::function<float()> level;
    float shownLevel = 0.0f;
    DrawnNumbers drawn;
    std::optional<Point> hovered, dragged;
    float dragTotal = 0.0f, dragHold = 0.0f; // the picture's time across, and its held sustain's, while a point is dragged
    float startTime = 0.0f, startSustain = 0.0f;

    // The stages across the picture: the attack, the decay, a held sustain and the release, each at least a little wide.
    struct Layout
    {
        juce::Rectangle<float> area;
        float attack = 0.0f, decay = 0.0f, hold = 0.0f, release = 0.0f, total = 1.0f;

        float x (float time) const { return area.getX() + time / total * area.getWidth(); }
        float y (float value) const { return area.getBottom() - value * area.getHeight(); }
    };

    Layout getLayout() const;
    std::optional<Point> findPoint (juce::Point<float>) const;
    ParameterHandle& getTimeOf (Point);
    void setHovered (std::optional<Point>);
    void tick() override;
};

// An LFO's wave, one cycle, and a dot where it is now. With its controls given, a drag up or down sets its depth (the wave
// drawn as tall as it moves) and the wheel its rate, or with its sync on, the note value, each one undo step.
class LfoView : public juce::Component,
                private DisplayTicker::Display
{
public:
    // shapeAt: the wave at a phase from 0 to 1, from -1 to 1; phase: where it is now, from 0 to 1 (below 0 for nowhere).
    LfoView (const juce::String& title, std::function<float (float phase)> shapeAt, std::function<float()> phase);

    // What a drag and the wheel change: the depth from 0 to 1 (none: drawn whole, and a drag changes nothing), the rate in
    // hertz, and while sync, a switch, is on, the division, a choice of note values.
    void setControls (ParameterHandle depth, ParameterHandle rate, ParameterHandle sync = {}, ParameterHandle division = {});

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr int drawnWavePoints = 64; // where a tick reads the wave, to see whether it changed

    std::function<float (float)> shapeAt;
    std::function<float()> phase;
    ParameterHandle depth, rate, sync, division;
    graph::WheelGesture wheel; // after the handles it turns
    float shownPhase = -1.0f, startDepth = 0.0f;
    DrawnNumbers drawn;

    float getDrawnDepth() const; // how tall the wave is drawn, from 0 to 1
    void tick() override;
};

// An LFO's wave as the synths' LFO views draw it, by its shape's number (in dsp::LfoShape's order): dsp::Lfo's own
// values, and the random one as steps.
float getDrawnLfoValue (int shape, float phase);

// A filter's response: at its cutoff as set, and, fainter, where the newest note plays it now; a dot at the cutoff, as high
// as the resonance, which a drag moves (both, as one undo step).
class FilterView : public juce::Component,
                   private DisplayTicker::Display
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
    DrawnNumbers drawn;

    graph::FrequencyAxis getFrequencies() const;
    void dragTo (juce::Point<float>);
    void tick() override;
};
} // namespace phazite::ui
