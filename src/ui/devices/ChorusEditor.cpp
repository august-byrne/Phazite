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

    constexpr float lowestGain = -36.0f, highestGain = 12.0f;                             // the response's levels, in decibels
    constexpr float rateStep = 0.05f;                                                     // a notch of the wheel, along the rate's knob
    constexpr int tapeHeight = 22;                                                        // the strip of the delay
    constexpr double shortestShown = 0.5, longestShown = 60.0;                            // its milliseconds, on a log scale
    constexpr float headRadius = 4.0f, smallHeadRadius = 3.0f, smallestHeadRadius = 2.5f; // for up to 3, 5 and 8 copies a side
    constexpr float fullFanSpread = 0.25f; // the Spread from which each copy has a line of its own on the strip
    constexpr float partHeight = 6.0f;     // the bar of the part of the sweep a copy has

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
                        "copy lags, in milliseconds. Drag the dot, or press anywhere to put it there: sideways for the delay, up or "
                        "down for the depth. The wheel turns the rate.");
    }

    int getNumVoices() const override { return Chorus::getNumVoices (values); }

    double getVoicePhase (int channel, int voice) const override { return Chorus::getVoicePhase (values, getLfoPhase(), channel, voice); }

    float getSweepValue (double at) const override { return Chorus::getSweep (values, at); }

    double getDelay (int channel, int voice) const { return Chorus::getVoiceDelay (values, getVoicePhase (channel, voice), voice); }

    // Where the strip of the delay shows a side's copy now.
    juce::Point<float> getHeadPosition (int channel, int voice) const
    {
        const auto strip = getStripArea();
        return { xForMilliseconds (strip.reduced ((float) Space::s, 0.0f), getDelay (channel, voice)), getLineY (strip, voice) };
    }

    juce::Point<float> getDotPosition() const override
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
        dot.paint (g, p, tint);

        graph::paintReadouts (g, p, area, formatMilliseconds (values[Chorus::delay]) + "  Depth " + toPercent (values[Chorus::depth]),
                              juce::String (getNumVoices()) + (getNumVoices() == 1 ? " voice" : " voices"));

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

    float getHeadRadius() const
    {
        const auto numVoices = getNumVoices();
        return numVoices <= 3 ? headRadius : (numVoices <= 5 ? smallHeadRadius : smallestHeadRadius);
    }

    // How far apart the copies' lines are on the strip of the delay: none while they all sweep the whole of it, and opening
    // as Spread gives each its part.
    float getLineGap (juce::Rectangle<float> strip) const
    {
        const auto numVoices = getNumVoices();

        if (numVoices <= 1)
            return 0.0f;

        const auto room = strip.getHeight() - 2.0f * getHeadRadius() - 2.0f;
        return juce::jlimit (0.0f, 1.0f, values[Chorus::spread] / fullFanSpread) * room / (float) (numVoices - 1);
    }

    float getLineY (juce::Rectangle<float> strip, int voice) const
    {
        return strip.getCentreY() + ((float) voice - 0.5f * (float) (getNumVoices() - 1)) * getLineGap (strip);
    }

    // The delay as a tape's heads: how far each copy's sweep reaches, and where each copy lags now.
    void paintTape (juce::Graphics& g, const Palette& p, juce::Colour tint) const
    {
        const auto strip = getStripArea();
        graph::paintBackground (g, p, strip);
        const auto inner = strip.reduced ((float) Space::s, 0.0f);

        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (const auto milliseconds : { 1.0, 2.0, 5.0, 10.0, 20.0, 50.0 })
            g.drawVerticalLine (juce::roundToInt (xForMilliseconds (inner, milliseconds)), strip.getY() + 2.0f, strip.getBottom() - 2.0f);

        // The part of the sweep each copy has: one bar while they share the whole of it, a bar each on its line otherwise.
        const auto gap = getLineGap (strip);
        const auto bar = gap > 0.0f ? juce::jlimit (2.0f, partHeight, gap - 1.0f) : partHeight;
        g.setColour (tint.withAlpha (GraphTint::partCurve));

        for (int voice = 0; voice < (gap > 0.0f ? getNumVoices() : 1); ++voice)
        {
            const auto reach = Chorus::getVoiceReach (values, voice);
            const auto shortest = xForMilliseconds (inner, reach.shortest), longest = xForMilliseconds (inner, reach.longest);
            g.fillRoundedRectangle ({ shortest, getLineY (strip, voice) - 0.5f * bar, std::max (2.0f, longest - shortest), bar },
                                    0.5f * bar);
        }

        const auto radius = getHeadRadius();

        for (int channel = 1; channel >= 0; --channel)
        {
            for (int voice = 0; voice < getNumVoices(); ++voice)
            {
                const auto head = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (getHeadPosition (channel, voice));
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
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    auto& shown = keep (std::make_unique<Picture> (device));
    picture = &shown;

    const auto sweep = makeControlsPart ("Sweep", { "delay", "depth" }, 2, ParameterControls::Size::large);
    const auto lfo = makeControlsPart ("LFO", { "sync", "division", "rate", "shape" }, 4, ParameterControls::Size::compact,
                                       { "Sync", "Note", "Rate", "Shape" });
    const auto copies = makeControlsPart ("Voices", { "voices", "voicePhase", "spread", "shimmer" }, 4, ParameterControls::Size::compact,
                                          { "Voices", "Phase", "Spread", "Shimmer" });
    const auto tone = makeControlsPart ("Tone", { "stereo", "feedback", "highCut", "safeBass" }, 4);
    setParts ({ { { "Comb", &shown, 320, 252, 166, true, GraphTint::minimumWidth } }, { sweep, lfo }, { copies, tone } });
    refresh();
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

juce::Point<float> ChorusEditor::getHeadPosition (int channel, int voice) const
{
    return picture->getHeadPosition (channel, voice);
}

void ChorusEditor::refresh()
{
    // One voice a side has no others to be a phase from, or to share the sweep with: those two controls faint.
    const auto severalVoices = juce::roundToInt (device.getParameter ("voices").getValue()) > 0;

    for (const auto* id : { "voicePhase", "spread" })
        if (auto* control = findControl (id))
            control->setAlpha (severalVoices ? 1.0f : SwitchTint::partOff);
}
} // namespace phazite::ui
