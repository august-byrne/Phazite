// SPDX-License-Identifier: AGPL-3.0-or-later
// What devices' displays share (docs/design.md, section 9): a graph's axes (frequency, level) and grid, curves, a spectrum
// of what a device plays, the last seconds of a level, and the gestures their handles make: a drag or a turn of the wheel,
// each one undo step.
#pragma once

#include "devices/core/SampleTap.h"
#include "devices/core/Spectrum.h"
#include "model/devices/ParameterHandle.h"
#include "ui/theme/DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace phazite::ui::graph
{
// Frequencies across an area, on a log scale: 20 Hz to 20 kHz unless given others.
struct FrequencyAxis
{
    juce::Rectangle<float> area;
    double low = 20.0, high = 20000.0;

    float xFor (double frequency) const;
    double frequencyAt (float x) const;
};

// Levels up an area, in decibels.
struct LevelAxis
{
    juce::Rectangle<float> area;
    float low = -60.0f, high = 0.0f;

    float yFor (float decibels) const;
    float levelAt (float y) const;
};

// A graph's background, rounded; and its grid: a line at 100 Hz, 1 kHz and 10 kHz (and the ones between, fainter), and one
// every step decibels, with their numbers where asked.
void paintBackground (juce::Graphics&, const Palette&, juce::Rectangle<float> area);
void paintFrequencyGrid (juce::Graphics&, const Palette&, const FrequencyAxis&, bool withNumbers);
void paintLevelGrid (juce::Graphics&, const Palette&, const LevelAxis&, float step, bool withNumbers);

// A curve across an area, y for each x, every step pixels.
juce::Path makeCurve (juce::Rectangle<float> area, const std::function<float (float x)>& yAt, float step = 1.5f);

// A short text in a corner of a graph, such as what the pointer is over ("1.2 kHz  +3.0 dB").
void paintReadout (juce::Graphics&, const Palette&, juce::Rectangle<float> area, const juce::String&, juce::Justification);

// "1.2 kHz", "350 Hz"; "+3.0 dB", "-12 dB".
juce::String formatFrequency (double hertz);
juce::String formatDecibels (float decibels);

//==============================================================================
// The spectrum of a device's sound (devices/core/Spectrum.h), as an analyzer shows it: a filled area and a line, each
// level tilted up 4.5 dB an octave around 1 kHz, as analyzers show music, so that a mix looks about flat.
class SpectrumTrace
{
public:
    explicit SpectrumTrace (int order = 12);

    void setSampleRate (double);

    // Takes the newest samples (both channels, averaged), then analyzes them; call at each refresh.
    void pull (devices::StereoTap&);
    void pull (devices::SampleTap&);
    void update();

    void paint (juce::Graphics&, const FrequencyAxis&, const LevelAxis&, juce::Colour fill, juce::Colour line) const;
    float getLevelAt (double frequency) const; // tilted, in decibels

    static constexpr float tiltPerOctave = 4.5f;

private:
    devices::Spectrum spectrum;
    std::vector<float> lefts, rights;
};

//==============================================================================
// The last seconds of a value, one a refresh, for a display that scrolls, such as a limiter's history of levels.
class LevelHistory
{
public:
    explicit LevelHistory (int length, float initialValue);

    void push (float);
    int size() const noexcept { return (int) values.size(); }
    float get (int index) const noexcept; // from the oldest (0) to the newest (size() - 1)

    // Across an area, the oldest at the left: a line through the values, or the area under it down to the bottom.
    juce::Path makeLine (juce::Rectangle<float> area, const std::function<float (float value)>& yFor) const;
    juce::Path makeArea (juce::Rectangle<float> area, const std::function<float (float value)>& yFor, float baseY) const;

private:
    std::vector<float> values;
    int next = 0;
};

//==============================================================================
// The wheel turning a parameter, such as an EQ band's Q, as one undo step until it rests for a moment. The handle turned
// is the caller's, and must outlive this (make it a member declared after the handles).
class WheelGesture : private juce::Timer
{
public:
    ~WheelGesture() override;

    void turn (ParameterHandle&, float newValue);
    void end();

private:
    ParameterHandle* turning = nullptr;
    void timerCallback() override { end(); }
};
} // namespace phazite::ui::graph
