// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FlangerEditor.h"
#include "SweepPicture.h"
#include "devices/motion/Flanger.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Flanger;

    constexpr float lowestGain = -36.0f, highestGain = 12.0f;  // the response's levels, in decibels
    constexpr float depthStep = 0.05f;                         // a notch of the wheel
    constexpr int tapeHeight = 22;                             // the strip of the delay
    constexpr double shortestShown = 0.1, longestShown = 16.0; // its milliseconds, on a log scale
    constexpr int mostTeeth = 160;                             // the comb's notches drawn, at most
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

class FlangerEditor::Picture : public SweepPicture
{
public:
    explicit Picture (const DeviceHandle& d)
        : SweepPicture (d, Flanger::info, tapeHeight),
          delay (device.getParameter ("delay")),
          feedback (device.getParameter ("feedback")),
          depth (device.getParameter ("depth"))
    {
        setTitle ("Flanger comb");
        setDescription ("Its comb of notches now, over the spectrum of what it plays, and under it how far the copy lags each side, "
                        "in milliseconds. Drag the dot sideways to change the delay and up or down for the "
                        "feedback; the wheel turns the depth.");
    }

    double getDelay (int channel) const { return Flanger::getSweptDelay (values, getLfoValue (channel)); }

    juce::Point<float> getDotPosition() const
    {
        return { getFrequencies().xFor (1000.0 / (double) values[Flanger::delay]), yForFeedback (values[Flanger::feedback]) };
    }

    // A drag moves the dot: the delay with it sideways (its frequency under the pointer) and the feedback up and down, as
    // one undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        delay.beginGesture();
        feedback.beginGesture();
        dragTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override { dragTo (e.position); }

    void mouseUp (const juce::MouseEvent&) override
    {
        feedback.endGesture();
        delay.endGesture();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto range = depth.getRange();
        wheelGesture.turn (depth, juce::jlimit (range.start, range.end, depth.getValue() + (wheel.deltaY > 0.0f ? depthStep : -depthStep)));
        readValues();
        repaint();
    }

protected:
    void paintResponse (juce::Graphics& g, const Palette& p, juce::Colour tint) override
    {
        const auto area = getResponseArea();
        const auto frequencies = getFrequencies();
        const graph::LevelAxis levels { area, lowestGain, highestGain };
        const auto rate = getSampleRate();
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        // The comb's teeth, where the left side has them now: a line down at each notch.
        const auto leftDelay = getDelay (0);
        const auto negative = values[Flanger::feedback] < 0.0f;
        const auto spacing = 1000.0 / leftDelay; // hertz between notches
        g.setColour (tint.withAlpha (GraphTint::grid * 1.5f));

        for (int k = 0; k < mostTeeth; ++k)
        {
            const auto notch = negative ? (k + 1) * spacing : (k + 0.5) * spacing;

            if (notch > frequencies.high)
                break;

            g.drawVerticalLine (juce::roundToInt (frequencies.xFor (notch)), area.getY(), area.getBottom());
        }

        // Each side's response: the right's faint, then the left's.
        for (int channel = 1; channel >= 0; --channel)
        {
            if (channel == 1 && values[Flanger::stereo] < 0.01f)
                continue;

            const auto milliseconds = channel == 0 ? leftDelay : getDelay (1);
            const auto curve =
                graph::makeCurve (area,
                                  [&] (float x)
                                  {
                                      const auto magnitude =
                                          Flanger::getMagnitude (values, milliseconds, frequencies.frequencyAt (x), rate);
                                      return levels.yFor ((float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0));
                                  });
            g.setColour (channel == 0 ? tint : tint.withAlpha (GraphTint::marker));
            g.strokePath (curve, juce::PathStrokeType (channel == 0 ? GraphTint::curveWidth : 1.0f));
        }

        // The dot: the longest delay's frequency, as high as the feedback.
        const auto dot = juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (getDotPosition());
        g.setColour (tint);
        g.fillEllipse (dot);
        g.setColour (p.text);
        g.drawEllipse (dot, 1.5f);

        graph::paintReadout (g, p, area, formatMilliseconds (values[Flanger::delay]) + "  Depth " + toPercent (values[Flanger::depth]),
                             juce::Justification::topLeft);
        graph::paintReadout (g, p, area, "Feedback " + toPercent (values[Flanger::feedback]), juce::Justification::topRight);

        paintTape (g, p, tint);
    }

    float readPhase() const override
    {
        auto* flanger = device.getProcessor<Flanger>();
        return flanger != nullptr ? flanger->getPhase().get() : 0.0f;
    }

private:
    ParameterHandle delay, feedback, depth;
    graph::WheelGesture wheelGesture; // after the handles it turns

    juce::Rectangle<float> getFeedbackArea() const { return getResponseArea().reduced (0.0f, GraphTint::dotRadius); }

    float yForFeedback (float value) const
    {
        const auto area = getFeedbackArea();
        const auto range = feedback.getRange();
        return juce::jmap (value, range.start, range.end, area.getBottom(), area.getY());
    }

    float xForMilliseconds (juce::Rectangle<float> area, double milliseconds) const
    {
        const auto at =
            std::log (juce::jlimit (shortestShown, longestShown, milliseconds) / shortestShown) / std::log (longestShown / shortestShown);
        return area.getX() + (float) at * area.getWidth();
    }

    // The delay as a tape's heads: how far the sweep reaches, and where each side's copy lags now.
    void paintTape (juce::Graphics& g, const Palette& p, juce::Colour tint) const
    {
        const auto strip = getStripArea();
        graph::paintBackground (g, p, strip);
        const auto inner = strip.reduced ((float) Space::s, 0.0f);

        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (const auto milliseconds : { 0.2, 0.5, 1.0, 2.0, 5.0, 10.0 })
            g.drawVerticalLine (juce::roundToInt (xForMilliseconds (inner, milliseconds)), strip.getY() + 2.0f, strip.getBottom() - 2.0f);

        const auto shortest = xForMilliseconds (inner, Flanger::getSweptDelay (values, -1.0f));
        const auto longest = xForMilliseconds (inner, Flanger::getSweptDelay (values, 1.0f));
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle ({ shortest, strip.getCentreY() - 3.0f, std::max (2.0f, longest - shortest), 6.0f }, 3.0f);

        for (int channel = 1; channel >= 0; --channel)
        {
            const auto head = juce::Rectangle<float> (2.0f * headRadius, 2.0f * headRadius)
                                  .withCentre ({ xForMilliseconds (inner, getDelay (channel)), strip.getCentreY() });
            g.setColour (tint);

            if (channel == 0)
                g.fillEllipse (head);
            else
                g.drawEllipse (head, 1.5f);
        }

        graph::paintReadout (g, p, strip, formatMilliseconds (getDelay (0)), juce::Justification::centredRight);
    }

    void dragTo (juce::Point<float> position)
    {
        const auto delayRange = delay.getRange();
        const auto firstTooth = std::max (1.0, getFrequencies().frequencyAt (position.x));
        delay.setValue (juce::jlimit (delayRange.start, delayRange.end, (float) (1000.0 / firstTooth)));

        const auto area = getFeedbackArea();
        const auto feedbackRange = feedback.getRange();
        const auto y = juce::jlimit (area.getY(), area.getBottom(), position.y);
        feedback.setValue (juce::jmap (y, area.getBottom(), area.getY(), feedbackRange.start, feedbackRange.end));
        readValues();
        repaint();
    }
};

//==============================================================================
FlangerEditor::FlangerEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Picture> (device));
    picture = &shown;

    const auto sweep = makeControlsPart ("Sweep", { "delay", "depth" }, 2, ParameterControls::Size::large);
    const auto lfo = makeControlsPart ("LFO", { "sync", "division", "rate", "shape" }, 4, ParameterControls::Size::compact,
                                       { "Sync", "Note", "Rate", "Shape" });
    const auto tone = makeControlsPart ("Tone", { "feedback", "stereo", "safeBass" }, 3);
    setParts ({ { { "Comb", &shown, 320, 252, 166, true } }, { sweep, lfo }, { tone } });
}

FlangerEditor::~FlangerEditor() = default;

juce::Component& FlangerEditor::getPicture() const noexcept
{
    return *picture;
}

juce::Point<float> FlangerEditor::getDotPosition() const
{
    return picture->getDotPosition();
}

double FlangerEditor::getShownDelay (int channel) const
{
    return picture->getDelay (channel);
}
} // namespace phazite::ui
