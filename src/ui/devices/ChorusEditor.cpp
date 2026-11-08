// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChorusEditor.h"
#include "SweepPicture.h"
#include "devices/motion/Chorus.h"
#include "ui/theme/LookAndFeel.h"

#include <array>
#include <cmath>
#include <span>

namespace phazite::ui
{
namespace
{
    using devices::Chorus;

    constexpr float lowestGain = -36.0f, highestGain = 12.0f;  // the response's levels, in decibels
    constexpr float rateStep = 0.05f;                          // a notch of the wheel, along the rate's knob
    constexpr int tapeHeight = 22;                             // the strip of the delay
    constexpr double shortestShown = 0.5, longestShown = 60.0; // its milliseconds, on a log scale
    constexpr float headRadius = 4.0f;

    juce::String toPercent (float value)
    {
        return juce::String (juce::roundToInt (value * 100.0f)) + "%";
    }

    juce::String formatMilliseconds (double milliseconds)
    {
        return juce::String (milliseconds, milliseconds < 10.0 ? 1 : 0) + " ms";
    }
} // namespace

class ChorusEditor::Picture : public SweepPicture
{
public:
    explicit Picture (const DeviceHandle& d)
        : SweepPicture (d, Chorus::info, tapeHeight),
          delay (device.getParameter ("delay")),
          depth (device.getParameter ("depth")),
          sync (device.getParameter ("sync")),
          division (device.getParameter ("division")),
          rate (device.getParameter ("rate"))
    {
        setTitle ("Chorus copies");
        setDescription ("The comb its copies make with the sound now, over the spectrum of what it plays, and under it how far each "
                        "copy lags, in milliseconds. Drag the dot sideways to change the delay and up or down for the depth; the "
                        "wheel turns the rate.");
    }

    int getNumVoices() const override { return Chorus::getNumVoices (values); }

    double getVoicePhase (int channel, int voice) const override { return Chorus::getVoicePhase (values, getLfoPhase(), channel, voice); }

    float getSweepValue (double at) const override { return Chorus::getSweep (values, at); }

    double getDelay (int channel, int voice) const { return Chorus::getVoiceDelay (values, getVoicePhase (channel, voice)); }

    juce::Point<float> getDotPosition() const
    {
        return { getFrequencies().xFor (1000.0 / (double) values[Chorus::delay]), yForDepth (values[Chorus::depth]) };
    }

    // A drag moves the dot: the delay with it sideways (its frequency under the pointer) and the depth up and down, as one
    // undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        delay.beginGesture();
        depth.beginGesture();
        dragTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override { dragTo (e.position); }

    void mouseUp (const juce::MouseEvent&) override
    {
        depth.endGesture();
        delay.endGesture();
    }

    // The wheel turns the rate faster or slower, or in time with the song steps through the note values.
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto faster = wheel.deltaY > 0.0f;

        if (sync.getValue() > 0.5f)
        {
            const auto range = division.getRange();
            wheelGesture.turn (division, juce::jlimit (range.start, range.end, division.getValue() + (faster ? -1.0f : 1.0f)));
        }
        else
        {
            const auto range = rate.getRange();
            const auto along = juce::jlimit (0.0f, 1.0f, range.convertTo0to1 (rate.getValue()) + (faster ? rateStep : -rateStep));
            wheelGesture.turn (rate, range.convertFrom0to1 (along));
        }

        readValues();
        repaint();
    }

protected:
    void paintResponse (juce::Graphics& g, const Palette& p, juce::Colour tint) override
    {
        const auto area = getResponseArea();
        const auto frequencies = getFrequencies();
        const graph::LevelAxis levels { area, lowestGain, highestGain };
        const auto sampleRate = getSampleRate();
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        // Each side's response, with its copies where they are now: the right's faint, then the left's.
        for (int channel = 1; channel >= 0; --channel)
        {
            if (channel == 1 && values[Chorus::stereo] < 0.01f)
                continue;

            std::array<double, Chorus::mostVoices> milliseconds {};

            for (int voice = 0; voice < getNumVoices(); ++voice)
                milliseconds[(size_t) voice] = getDelay (channel, voice);

            const std::span<const double> copies (milliseconds.data(), (size_t) getNumVoices());
            const auto curve =
                graph::makeCurve (area,
                                  [&] (float x)
                                  {
                                      const auto magnitude = Chorus::getMagnitude (values, copies, frequencies.frequencyAt (x), sampleRate);
                                      return levels.yFor ((float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0));
                                  });
            g.setColour (channel == 0 ? tint : tint.withAlpha (GraphTint::marker));
            g.strokePath (curve, juce::PathStrokeType (channel == 0 ? GraphTint::curveWidth : 1.0f));
        }

        // The dot: the delay's frequency, as high as the depth.
        const auto dot = juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (getDotPosition());
        g.setColour (tint);
        g.fillEllipse (dot);
        g.setColour (p.text);
        g.drawEllipse (dot, 1.5f);

        const auto mode = Chorus::info.parameters[Chorus::mode].choices[(size_t) (getNumVoices() > 1 ? 1 : 0)];
        graph::paintReadouts (g, p, area, formatMilliseconds (values[Chorus::delay]) + "  Depth " + toPercent (values[Chorus::depth]),
                              juce::String (mode.data(), mode.size()));

        paintTape (g, p, tint);
    }

    float readPhase() const override
    {
        auto* chorus = device.getProcessor<Chorus>();
        return chorus != nullptr ? chorus->getPhase().get() : 0.0f;
    }

private:
    ParameterHandle delay, depth, sync, division, rate;
    graph::WheelGesture wheelGesture; // after the handles it turns

    juce::Rectangle<float> getDepthArea() const { return getResponseArea().reduced (0.0f, GraphTint::dotRadius); }

    float yForDepth (float value) const
    {
        const auto area = getDepthArea();
        const auto range = depth.getRange();
        return juce::jmap (value, range.start, range.end, area.getBottom(), area.getY());
    }

    float xForMilliseconds (juce::Rectangle<float> area, double milliseconds) const
    {
        const auto at =
            std::log (juce::jlimit (shortestShown, longestShown, milliseconds) / shortestShown) / std::log (longestShown / shortestShown);
        return area.getX() + (float) at * area.getWidth();
    }

    // The delay as a tape's heads: how far the sweep reaches, and where each copy lags now.
    void paintTape (juce::Graphics& g, const Palette& p, juce::Colour tint) const
    {
        const auto strip = getStripArea();
        graph::paintBackground (g, p, strip);
        const auto inner = strip.reduced ((float) Space::s, 0.0f);

        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (const auto milliseconds : { 1.0, 2.0, 5.0, 10.0, 20.0, 50.0 })
            g.drawVerticalLine (juce::roundToInt (xForMilliseconds (inner, milliseconds)), strip.getY() + 2.0f, strip.getBottom() - 2.0f);

        const auto middle = (double) values[Chorus::delay], reach = middle * (double) values[Chorus::depth];
        const auto shortest = xForMilliseconds (inner, middle - reach), longest = xForMilliseconds (inner, middle + reach);
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle ({ shortest, strip.getCentreY() - 3.0f, std::max (2.0f, longest - shortest), 6.0f }, 3.0f);

        for (int channel = 1; channel >= 0; --channel)
        {
            for (int voice = 0; voice < getNumVoices(); ++voice)
            {
                const auto head = juce::Rectangle<float> (2.0f * headRadius, 2.0f * headRadius)
                                      .withCentre ({ xForMilliseconds (inner, getDelay (channel, voice)), strip.getCentreY() });
                g.setColour (tint);

                if (channel == 0)
                    g.fillEllipse (head);
                else
                    g.drawEllipse (head, 1.5f);
            }
        }

        graph::paintReadout (g, p, strip, formatMilliseconds (getDelay (0, 0)), juce::Justification::centredRight);
    }

    void dragTo (juce::Point<float> position)
    {
        const auto delayRange = delay.getRange();
        const auto frequency = std::max (1.0, getFrequencies().frequencyAt (position.x));
        delay.setValue (juce::jlimit (delayRange.start, delayRange.end, (float) (1000.0 / frequency)));

        const auto area = getDepthArea();
        const auto depthRange = depth.getRange();
        const auto y = juce::jlimit (area.getY(), area.getBottom(), position.y);
        depth.setValue (juce::jmap (y, area.getBottom(), area.getY(), depthRange.start, depthRange.end));
        readValues();
        repaint();
    }
};

//==============================================================================
ChorusEditor::ChorusEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Picture> (device));
    picture = &shown;

    const auto sweep = makeControlsPart ("Sweep", { "delay", "depth" }, 2, ParameterControls::Size::large);
    const auto lfo = makeControlsPart ("LFO", { "sync", "division", "rate", "shape" }, 4, ParameterControls::Size::compact,
                                       { "Sync", "Note", "Rate", "Shape" });
    const auto copies = makeControlsPart ("Copies", { "mode", "stereo", "feedback" }, 3);
    const auto tone = makeControlsPart ("Tone", { "highCut", "safeBass" }, 2);
    setParts ({ { { "Comb", &shown, 320, 252, 166, true, GraphTint::minimumWidth } }, { sweep, lfo }, { copies, tone } });
}

ChorusEditor::~ChorusEditor() = default;

juce::Component& ChorusEditor::getPicture() const noexcept
{
    return *picture;
}

juce::Point<float> ChorusEditor::getDotPosition() const
{
    return picture->getDotPosition();
}

int ChorusEditor::getShownVoices() const
{
    return picture->getNumVoices();
}

double ChorusEditor::getShownDelay (int channel, int voice) const
{
    return picture->getDelay (channel, voice);
}
} // namespace phazite::ui
