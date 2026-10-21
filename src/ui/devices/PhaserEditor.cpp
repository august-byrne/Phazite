// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PhaserEditor.h"
#include "SweepPicture.h"
#include "devices/motion/Phaser.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Phaser;

    constexpr float lowestGain = -36.0f, highestGain = 18.0f; // the response's levels, in decibels
    constexpr float depthStep = 0.05f;                        // a notch of the wheel
    constexpr float reachHeight = 6.0f;                       // the sweep's reach, along the bottom

    juce::String toPercent (float value)
    {
        return juce::String (juce::roundToInt (value * 100.0f)) + "%";
    }
} // namespace

class PhaserEditor::Picture : public SweepPicture
{
public:
    explicit Picture (const DeviceHandle& d)
        : SweepPicture (d, Phaser::info),
          frequency (device.getParameter ("frequency")),
          feedback (device.getParameter ("feedback")),
          depth (device.getParameter ("depth"))
    {
        setTitle ("Phaser notches");
        setDescription ("Where its notches are now, over the spectrum of what it plays, and how far the sweep reaches. Drag the "
                        "dot sideways to move the frequency and up or down for the feedback; the wheel turns the depth.");
    }

    double getCentre (int channel) const { return Phaser::getSweptCentre (values, getLfoValue (channel), getSampleRate()); }

    juce::Point<float> getDotPosition() const
    {
        return { getFrequencies().xFor (values[Phaser::frequency]), yForFeedback (values[Phaser::feedback]) };
    }

    // A drag moves the dot, the frequency with it sideways and the feedback up and down, as one undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        frequency.beginGesture();
        feedback.beginGesture();
        dragTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override { dragTo (e.position); }

    void mouseUp (const juce::MouseEvent&) override
    {
        feedback.endGesture();
        frequency.endGesture();
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

        // How far the sweep reaches, from its lowest centre to its highest.
        const auto lowX = frequencies.xFor (Phaser::getSweptCentre (values, -1.0f, rate));
        const auto highX = frequencies.xFor (Phaser::getSweptCentre (values, 1.0f, rate));
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle ({ lowX, area.getBottom() - reachHeight - 2.0f, std::max (2.0f, highX - lowX), reachHeight }, 2.0f);

        // Each side's response where the LFO has it now: the right's faint, then the left's, glowing down to the bottom.
        for (int channel = 1; channel >= 0; --channel)
        {
            if (channel == 1 && values[Phaser::stereo] < 0.01f)
                continue;

            const auto centre = getCentre (channel);
            const auto curve =
                graph::makeCurve (area,
                                  [&] (float x)
                                  {
                                      const auto magnitude = Phaser::getMagnitude (values, centre, frequencies.frequencyAt (x), rate);
                                      return levels.yFor ((float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0));
                                  });

            if (channel == 0)
            {
                auto glow = curve;
                glow.lineTo (area.getBottomRight());
                glow.lineTo (area.getBottomLeft());
                glow.closeSubPath();
                g.setGradientFill (juce::ColourGradient (tint.withAlpha (GraphTint::partCurve), area.getTopLeft(), tint.withAlpha (0.0f),
                                                         area.getBottomLeft(), false));
                g.fillPath (glow);
            }

            g.setColour (channel == 0 ? tint : tint.withAlpha (GraphTint::marker));
            g.strokePath (curve, juce::PathStrokeType (channel == 0 ? GraphTint::curveWidth : 1.0f));
        }

        // The dot: the frequency, as high as the feedback.
        const auto dot = juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (getDotPosition());
        g.setColour (tint);
        g.fillEllipse (dot);
        g.setColour (p.text);
        g.drawEllipse (dot, 1.5f);

        const auto notches = juce::roundToInt (values[Phaser::notches]) + 1;
        graph::paintReadout (g, p, area,
                             juce::String (notches) + (notches == 1 ? " notch  " : " notches  ")
                                 + graph::formatFrequency (values[Phaser::frequency]) + "  Depth " + toPercent (values[Phaser::depth]),
                             juce::Justification::topLeft);
        graph::paintReadout (g, p, area, "Feedback " + toPercent (values[Phaser::feedback]), juce::Justification::topRight);
    }

    float readPhase() const override
    {
        auto* phaser = device.getProcessor<Phaser>();
        return phaser != nullptr ? phaser->getPhase().get() : 0.0f;
    }

private:
    ParameterHandle frequency, feedback, depth;
    graph::WheelGesture wheelGesture; // after the handles it turns

    // The feedback's range up the response: its most at the top, its least at the bottom, none in the middle.
    juce::Rectangle<float> getFeedbackArea() const { return getResponseArea().reduced (0.0f, GraphTint::dotRadius); }

    float yForFeedback (float value) const
    {
        const auto area = getFeedbackArea();
        const auto range = feedback.getRange();
        return juce::jmap (value, range.start, range.end, area.getBottom(), area.getY());
    }

    void dragTo (juce::Point<float> position)
    {
        const auto frequencyRange = frequency.getRange();
        frequency.setValue (juce::jlimit (frequencyRange.start, frequencyRange.end, (float) getFrequencies().frequencyAt (position.x)));

        const auto area = getFeedbackArea();
        const auto feedbackRange = feedback.getRange();
        const auto y = juce::jlimit (area.getY(), area.getBottom(), position.y);
        feedback.setValue (juce::jmap (y, area.getBottom(), area.getY(), feedbackRange.start, feedbackRange.end));
        readValues();
        repaint();
    }
};

//==============================================================================
PhaserEditor::PhaserEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Picture> (device));
    picture = &shown;

    const auto sweep = makeControlsPart ("Sweep", { "frequency", "depth" }, 2, ParameterControls::Size::large);
    const auto lfo = makeControlsPart ("LFO", { "sync", "division", "rate", "shape" }, 4, ParameterControls::Size::compact,
                                       { "Sync", "Note", "Rate", "Shape" });
    const auto tone = makeControlsPart ("Tone", { "notches", "feedback", "spacing" }, 3);
    const auto space = makeControlsPart ("Stereo and Bass", { "stereo", "safeBass" }, 2);
    setParts ({ { { "Notches", &shown, 320, 226, 140, true } }, { sweep, lfo }, { tone, space } });
}

PhaserEditor::~PhaserEditor() = default;

juce::Component& PhaserEditor::getPicture() const noexcept
{
    return *picture;
}

juce::Point<float> PhaserEditor::getDotPosition() const
{
    return picture->getDotPosition();
}

double PhaserEditor::getShownCentre (int channel) const
{
    return picture->getCentre (channel);
}
} // namespace phazite::ui
