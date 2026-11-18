// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MotionEditor.h"
#include "SweepPicture.h"
#include "devices/motion/Motion.h"
#include "ui/theme/LookAndFeel.h"

#include <array>
#include <cmath>
#include <span>

namespace phazite::ui
{
namespace
{
    using devices::Chorus;
    using devices::Flanger;
    using devices::Motion;
    using devices::Phaser;

    constexpr float rateStep = 0.05f;  // a notch of the wheel, along the chorus's rate's knob
    constexpr float depthStep = 0.05f; // a notch of the wheel, along the flanger's and the phaser's depth
    constexpr int tapeHeight = 22;     // the strip of the delay, the chorus's and the flanger's
    constexpr float headRadius = 4.0f, smallHeadRadius = 3.0f, smallestHeadRadius = 2.5f; // for up to 3, 5 and 8 copies a side
    constexpr float fullFanSpread = 0.25f; // the Spread from which each of the chorus's copies has a line of its own on the strip
    constexpr float partHeight = 6.0f;     // the bar of the part of the sweep a copy has
    constexpr float reachHeight = 6.0f;    // the phaser's sweep's reach, along the bottom
    constexpr int mostTeeth = 160;         // the flanger's comb's notches drawn, at most

    juce::String toPercent (float value)
    {
        return juce::String (juce::roundToInt (value * 100.0f)) + "%";
    }

    juce::String formatMilliseconds (double milliseconds)
    {
        return juce::String (milliseconds, milliseconds < 10.0 ? 1 : 0) + " ms";
    }

    // Milliseconds along a strip, on a log scale between the shortest and the longest shown.
    float xForMilliseconds (juce::Rectangle<float> area, double milliseconds, double shortest, double longest)
    {
        const auto at = std::log (juce::jlimit (shortest, longest, milliseconds) / shortest) / std::log (longest / shortest);
        return area.getX() + (float) at * area.getWidth();
    }

    // Each mode's page: what the tab says it is.
    const std::vector<juce::String> modeTips {
        "Chorus: copies of the sound a few milliseconds behind it, their delays swept, so that they waver around it as players "
        "never quite in tune do",
        "Flanger: one copy of the sound, its delay swept down from the Delay, so that a comb of notches slides through it, the "
        "jet-plane whoosh",
        "Phaser: the sound through all-pass filters whose notches the LFO sweeps up and down its spectrum, around the Frequency",
    };

    // A value a dot moves by its height on a picture's response, its range from the bottom to the top: the chorus's depth,
    // the flanger's and the phaser's feedback (0 in the middle).
    class HeightAxis
    {
    public:
        HeightAxis (const SweepPicture& p, const ParameterHandle& h)
            : picture (p),
              handle (h)
        {
        }

        float yFor (float value) const
        {
            const auto area = getArea();
            const auto range = handle.getRange();
            return juce::jmap (value, range.start, range.end, area.getBottom(), area.getY());
        }

        float valueAt (float y) const
        {
            const auto area = getArea();
            const auto range = handle.getRange();
            return juce::jmap (juce::jlimit (area.getY(), area.getBottom(), y), area.getBottom(), area.getY(), range.start, range.end);
        }

    private:
        const SweepPicture& picture;
        const ParameterHandle& handle;

        juce::Rectangle<float> getArea() const { return picture.getResponseArea().reduced (0.0f, GraphTint::dotRadius); }
    };
} // namespace

//==============================================================================
class MotionEditor::ChorusPicture : public SweepPicture
{
public:
    explicit ChorusPicture (const DeviceHandle& d)
        : SweepPicture (d, Motion::info, tapeHeight),
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

        // A drag moves the dot, as one undo step.
        dot.setDrag (
            { &delay, [this] (float milliseconds) { return getFrequencies().xFor (1000.0 / (double) milliseconds); },
              [this] (float x) { return delayAt (x); } },
            { &depth, [this] (float value) { return depthAxis.yFor (value); }, [this] (float y) { return depthAxis.valueAt (y); } },
            [this]
            {
                readValues();
                repaint();
            });
    }

    std::array<float, Chorus::mix + 1> getEngineValues() const { return Motion::getChorusValues (values); }

    int getNumVoices() const override { return Chorus::getNumVoices (getEngineValues()); }

    double getVoicePhase (int channel, int voice) const override
    {
        return Chorus::getVoicePhase (getEngineValues(), getLfoPhase(), channel, voice);
    }

    float getSweepValue (double at) const override { return Chorus::getSweep (getEngineValues(), at); }

    double getDelay (int channel, int voice) const
    {
        const auto engine = getEngineValues();
        return Chorus::getVoiceDelay (engine, Chorus::getVoicePhase (engine, getLfoPhase(), channel, voice), voice);
    }

    // Where the strip of the delay shows a side's copy now.
    juce::Point<float> getHeadPosition (int channel, int voice) const
    {
        const auto strip = getStripArea();
        return { xFor (strip.reduced ((float) Space::s, 0.0f), getDelay (channel, voice)), getLineY (strip, voice) };
    }

    juce::Point<float> getDotPosition() const override
    {
        return { getFrequencies().xFor (1000.0 / (double) values[Motion::delay]), depthAxis.yFor (values[Motion::depth]) };
    }

    // The wheel turns the rate faster or slower, or in time with the song steps through the note values.
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto faster = wheel.deltaY > 0.0f;

        if (sync.getValue() > 0.5f)
        {
            const auto range = division.getRange();
            wheelGesture.turn (division, juce::jlimit (range.start, range.end, division.getBaseValue() + (faster ? -1.0f : 1.0f)));
        }
        else
        {
            const auto range = rate.getRange();
            const auto along = juce::jlimit (0.0f, 1.0f, range.convertTo0to1 (rate.getBaseValue()) + (faster ? rateStep : -rateStep));
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
        const auto engine = getEngineValues();
        const auto numVoices = Chorus::getNumVoices (engine);
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        // Each side's response, with its copies where they are now: the right's faint, then the left's.
        for (int channel = 1; channel >= 0; --channel)
        {
            if (channel == 1 && values[Motion::stereo] < 0.01f)
                continue;

            std::array<double, Chorus::mostVoices> milliseconds {};

            for (int voice = 0; voice < numVoices; ++voice)
                milliseconds[(size_t) voice] = getDelay (channel, voice);

            const std::span<const double> copies (milliseconds.data(), (size_t) numVoices);
            const auto curve =
                graph::makeCurve (area,
                                  [&] (float x)
                                  {
                                      const auto magnitude = Chorus::getMagnitude (engine, copies, frequencies.frequencyAt (x), sampleRate);
                                      return levels.yFor ((float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0));
                                  });
            g.setColour (channel == 0 ? tint : tint.withAlpha (GraphTint::marker));
            g.strokePath (curve, juce::PathStrokeType (channel == 0 ? GraphTint::curveWidth : 1.0f));
        }

        // The dot: the delay's frequency, as high as the depth.
        dot.paint (g, p, tint);

        graph::paintReadouts (g, p, area, formatMilliseconds (values[Motion::delay]) + "  Depth " + toPercent (values[Motion::depth]),
                              juce::String (numVoices) + (numVoices == 1 ? " voice" : " voices"));

        paintTape (g, p, tint);
    }

    float readPhase() const override
    {
        auto* motion = device.getProcessor<Motion>();
        return motion != nullptr ? motion->getChorus().getPhase().get() : 0.0f;
    }

private:
    static constexpr float lowestGain = -36.0f, highestGain = 12.0f;  // the response's levels, in decibels
    static constexpr double shortestShown = 0.2, longestShown = 60.0; // its milliseconds, on a log scale

    ParameterHandle delay, depth, sync, division, rate;
    HeightAxis depthAxis { *this, depth };
    graph::WheelGesture wheelGesture { dot.getDrag() }; // after the handles it turns, and the dot (in the base) that drags them

    static float xFor (juce::Rectangle<float> area, double milliseconds)
    {
        return xForMilliseconds (area, milliseconds, shortestShown, longestShown);
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
        return juce::jlimit (0.0f, 1.0f, values[Motion::spread] / fullFanSpread) * room / (float) (numVoices - 1);
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
        const auto engine = getEngineValues();

        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (const auto milliseconds : { 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0 })
            g.drawVerticalLine (juce::roundToInt (xFor (inner, milliseconds)), strip.getY() + 2.0f, strip.getBottom() - 2.0f);

        // The part of the sweep each copy has: one bar while they share the whole of it, a bar each on its line otherwise.
        const auto gap = getLineGap (strip);
        const auto bar = gap > 0.0f ? juce::jlimit (2.0f, partHeight, gap - 1.0f) : partHeight;
        g.setColour (tint.withAlpha (GraphTint::partCurve));

        for (int voice = 0; voice < (gap > 0.0f ? getNumVoices() : 1); ++voice)
        {
            const auto reach = Chorus::getVoiceReach (engine, voice);
            const auto shortest = xFor (inner, reach.shortest), longest = xFor (inner, reach.longest);
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

    // What the dot moves, for a drag: the delay with it sideways (its frequency under the pointer).
    float delayAt (float x) const { return (float) (1000.0 / std::max (1.0, getFrequencies().frequencyAt (x))); }
};

//==============================================================================
class MotionEditor::FlangerPicture : public SweepPicture
{
public:
    explicit FlangerPicture (const DeviceHandle& d)
        : SweepPicture (d, Motion::info, tapeHeight),
          delay (device.getParameter ("delay")),
          feedback (device.getParameter ("feedback")),
          depth (device.getParameter ("depth"))
    {
        setTitle ("Flanger comb");
        setDescription ("Its comb of notches now, over the spectrum of what it plays, and under it how far the copy lags each side, "
                        "in milliseconds. Drag the dot, or press anywhere to put it there: sideways for the delay, up or down for "
                        "the feedback. The wheel turns the depth.");

        // A drag moves the dot, as one undo step.
        dot.setDrag ({ &delay, [this] (float milliseconds) { return getFrequencies().xFor (1000.0 / (double) milliseconds); },
                       [this] (float x) { return delayAt (x); } },
                     { &feedback, [this] (float value) { return feedbackAxis.yFor (value); },
                       [this] (float y) { return feedbackAxis.valueAt (y); } },
                     [this]
                     {
                         readValues();
                         repaint();
                     });
    }

    double getDelay (int channel) const { return Flanger::getSweptDelay (Motion::getFlangerValues (values), getLfoValue (channel)); }

    juce::Point<float> getDotPosition() const override
    {
        return { getFrequencies().xFor (1000.0 / (double) values[Motion::delay]), feedbackAxis.yFor (values[Motion::feedback]) };
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto range = depth.getRange();
        wheelGesture.turn (depth,
                           juce::jlimit (range.start, range.end, depth.getBaseValue() + (wheel.deltaY > 0.0f ? depthStep : -depthStep)));
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
        const auto engine = Motion::getFlangerValues (values);
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        // The comb's teeth, where the left side has them now: a line down at each notch.
        const auto leftDelay = getDelay (0);
        const auto negative = engine[Flanger::feedback] < 0.0f;
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
            if (channel == 1 && values[Motion::stereo] < 0.01f)
                continue;

            const auto milliseconds = channel == 0 ? leftDelay : getDelay (1);
            const auto curve =
                graph::makeCurve (area,
                                  [&] (float x)
                                  {
                                      const auto magnitude =
                                          Flanger::getMagnitude (engine, milliseconds, frequencies.frequencyAt (x), rate);
                                      return levels.yFor ((float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0));
                                  });
            g.setColour (channel == 0 ? tint : tint.withAlpha (GraphTint::marker));
            g.strokePath (curve, juce::PathStrokeType (channel == 0 ? GraphTint::curveWidth : 1.0f));
        }

        // The dot: the longest delay's frequency, as high as the feedback.
        dot.paint (g, p, tint);

        graph::paintReadouts (g, p, area, formatMilliseconds (values[Motion::delay]) + "  Depth " + toPercent (values[Motion::depth]),
                              "Feedback " + toPercent (values[Motion::feedback]));

        paintTape (g, p, tint);
    }

    float readPhase() const override
    {
        auto* motion = device.getProcessor<Motion>();
        return motion != nullptr ? motion->getFlanger().getPhase().get() : 0.0f;
    }

private:
    static constexpr float lowestGain = -36.0f, highestGain = 12.0f;  // the response's levels, in decibels
    static constexpr double shortestShown = 0.1, longestShown = 30.0; // its milliseconds, on a log scale

    ParameterHandle delay, feedback, depth;
    HeightAxis feedbackAxis { *this, feedback };
    graph::WheelGesture wheelGesture { dot.getDrag() }; // after the handles it turns, and the dot (in the base) that drags them

    static float xFor (juce::Rectangle<float> area, double milliseconds)
    {
        return xForMilliseconds (area, milliseconds, shortestShown, longestShown);
    }

    // The delay as a tape's heads: how far the sweep reaches, and where each side's copy lags now.
    void paintTape (juce::Graphics& g, const Palette& p, juce::Colour tint) const
    {
        const auto strip = getStripArea();
        graph::paintBackground (g, p, strip);
        const auto inner = strip.reduced ((float) Space::s, 0.0f);
        const auto engine = Motion::getFlangerValues (values);

        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (const auto milliseconds : { 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0 })
            g.drawVerticalLine (juce::roundToInt (xFor (inner, milliseconds)), strip.getY() + 2.0f, strip.getBottom() - 2.0f);

        const auto shortest = xFor (inner, Flanger::getSweptDelay (engine, -1.0f));
        const auto longest = xFor (inner, Flanger::getSweptDelay (engine, 1.0f));
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle ({ shortest, strip.getCentreY() - 3.0f, std::max (2.0f, longest - shortest), 6.0f }, 3.0f);

        for (int channel = 1; channel >= 0; --channel)
        {
            const auto head = juce::Rectangle<float> (2.0f * headRadius, 2.0f * headRadius)
                                  .withCentre ({ xFor (inner, getDelay (channel)), strip.getCentreY() });
            g.setColour (tint);

            if (channel == 0)
                g.fillEllipse (head);
            else
                g.drawEllipse (head, 1.5f);
        }

        graph::paintReadout (g, p, strip, formatMilliseconds (getDelay (0)), juce::Justification::centredRight);
    }

    // What the dot moves, for a drag: the delay with it sideways (its first tooth's frequency under the pointer).
    float delayAt (float x) const { return (float) (1000.0 / std::max (1.0, getFrequencies().frequencyAt (x))); }
};

//==============================================================================
class MotionEditor::PhaserPicture : public SweepPicture
{
public:
    explicit PhaserPicture (const DeviceHandle& d)
        : SweepPicture (d, Motion::info),
          frequency (device.getParameter ("frequency")),
          feedback (device.getParameter ("feedback")),
          depth (device.getParameter ("depth"))
    {
        setTitle ("Phaser notches");
        setDescription ("Where its notches are now, over the spectrum of what it plays, and how far the sweep reaches. Drag the "
                        "dot, or press anywhere to put it there: sideways for the frequency, up or down for the feedback. The "
                        "wheel turns the depth.");

        // A drag moves the dot, the frequency with it sideways and the feedback up and down, as one undo step.
        dot.setDrag ({ &frequency, [this] (float hertz) { return getFrequencies().xFor (hertz); },
                       [this] (float x) { return (float) getFrequencies().frequencyAt (x); } },
                     { &feedback, [this] (float value) { return feedbackAxis.yFor (value); },
                       [this] (float y) { return feedbackAxis.valueAt (y); } },
                     [this]
                     {
                         readValues();
                         repaint();
                     });
    }

    double getCentre (int channel) const
    {
        return Phaser::getSweptCentre (Motion::getPhaserValues (values), getLfoValue (channel), getSampleRate());
    }

    juce::Point<float> getDotPosition() const override
    {
        return { getFrequencies().xFor (values[Motion::frequency]), feedbackAxis.yFor (values[Motion::feedback]) };
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto range = depth.getRange();
        wheelGesture.turn (depth,
                           juce::jlimit (range.start, range.end, depth.getBaseValue() + (wheel.deltaY > 0.0f ? depthStep : -depthStep)));
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
        const auto engine = Motion::getPhaserValues (values);
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        // How far the sweep reaches, from its lowest centre to its highest.
        const auto lowX = frequencies.xFor (Phaser::getSweptCentre (engine, -1.0f, rate));
        const auto highX = frequencies.xFor (Phaser::getSweptCentre (engine, 1.0f, rate));
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle ({ lowX, area.getBottom() - reachHeight - 2.0f, std::max (2.0f, highX - lowX), reachHeight }, 2.0f);

        // Each side's response where the LFO has it now: the right's faint, then the left's, glowing down to the bottom.
        for (int channel = 1; channel >= 0; --channel)
        {
            if (channel == 1 && values[Motion::stereo] < 0.01f)
                continue;

            const auto centre = getCentre (channel);
            const auto curve =
                graph::makeCurve (area,
                                  [&] (float x)
                                  {
                                      const auto magnitude = Phaser::getMagnitude (engine, centre, frequencies.frequencyAt (x), rate);
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
        dot.paint (g, p, tint);

        const auto notches = juce::roundToInt (values[Motion::notches]) + 1;
        graph::paintReadouts (g, p, area,
                              juce::String (notches) + (notches == 1 ? " notch  " : " notches  ")
                                  + graph::formatFrequency (values[Motion::frequency]) + "  Depth " + toPercent (values[Motion::depth]),
                              "Feedback " + toPercent (values[Motion::feedback]));
    }

    float readPhase() const override
    {
        auto* motion = device.getProcessor<Motion>();
        return motion != nullptr ? motion->getPhaser().getPhase().get() : 0.0f;
    }

private:
    static constexpr float lowestGain = -36.0f, highestGain = 18.0f; // the response's levels, in decibels

    ParameterHandle frequency, feedback, depth;
    HeightAxis feedbackAxis { *this, feedback };
    graph::WheelGesture wheelGesture { dot.getDrag() }; // after the handles it turns, and the dot (in the base) that drags them
};

//==============================================================================
MotionEditor::MotionEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    chorus = &keep (std::make_unique<ChorusPicture> (device));
    flanger = &keep (std::make_unique<FlangerPicture> (device));
    phaser = &keep (std::make_unique<PhaserPicture> (device));

    // What the three modes share: the LFO, and the Stereo and Bass part, the same controls in each.
    const auto lfo = makeControlsPart ("LFO", { "sync", "division", "rate", "shape" }, 4, ParameterControls::Size::compact,
                                       { "Sync", "Note", "Rate", "Shape" });
    const auto space = makeControlsPart ("Stereo and Bass", { "stereo", "safeBass" }, 2);

    // Chorus: its copies, swept by its delay and depth, its voices, its tone.
    const DeviceLayout::Part chorusPart { "Comb", chorus, 320, 252, 166, true, GraphTint::minimumWidth };
    const auto delaySweep = makeControlsPart ("Sweep", { "delay", "depth" }, 2, ParameterControls::Size::large);
    const auto voices = makeControlsPart ("Voices", { "voices", "voicePhase", "spread", "shimmer" }, 4, ParameterControls::Size::compact,
                                          { "Voices", "Phase", "Spread", "Shimmer" });
    const auto chorusTone = makeControlsPart ("Tone", { "feedback", "highCut" }, 2);

    // Flanger: its comb, swept by its delay and depth, its feedback.
    const DeviceLayout::Part flangerPart { "Comb", flanger, 320, 252, 166, true, GraphTint::minimumWidth };
    const auto flangerTone = makeControlsPart ("Tone", { "feedback" }, 1);

    // Phaser: its notches, swept around its frequency by its depth, their number, feedback and spacing.
    const DeviceLayout::Part phaserPart { "Notches", phaser, 320, 226, 140, true, GraphTint::minimumWidth };
    const auto frequencySweep = makeControlsPart ("Sweep", { "frequency", "depth" }, 2, ParameterControls::Size::large);
    const auto phaserTone = makeControlsPart ("Tone", { "notches", "feedback", "spacing" }, 3);

    setModes (Motion::info.parameters[Motion::mode].id,
              { { { chorusPart }, { delaySweep, lfo }, { voices, chorusTone, space } },
                { { flangerPart }, { delaySweep, lfo }, { flangerTone, space } },
                { { phaserPart }, { frequencySweep, lfo }, { phaserTone, space } } },
              modeTips);
    refresh();
}

MotionEditor::~MotionEditor() = default;

juce::Component& MotionEditor::getPicture() const noexcept
{
    switch ((Motion::Mode) getShownMode())
    {
        case Motion::Mode::flanger:
            return *flanger;
        case Motion::Mode::phaser:
            return *phaser;
        case Motion::Mode::chorus:
            break;
    }

    return *chorus;
}

juce::Point<float> MotionEditor::getDotPosition() const
{
    return dynamic_cast<const SweepPicture&> (getPicture()).getDotPosition();
}

int MotionEditor::getShownVoices() const
{
    return chorus->getNumVoices();
}

double MotionEditor::getShownDelay (int channel, int voice) const
{
    return getShownMode() == (int) Motion::Mode::flanger ? flanger->getDelay (channel) : chorus->getDelay (channel, voice);
}

juce::Point<float> MotionEditor::getHeadPosition (int channel, int voice) const
{
    return chorus->getHeadPosition (channel, voice);
}

double MotionEditor::getShownCentre (int channel) const
{
    return phaser->getCentre (channel);
}

void MotionEditor::refresh()
{
    // One voice a side has no others to be a phase from, or to share the sweep with: those two controls faint.
    const auto severalVoices = juce::roundToInt (device.getParameter ("voices").getValue()) > 0;

    for (const auto* id : { "voicePhase", "spread" })
        if (auto* control = findControl (id))
            control->setAlpha (severalVoices ? 1.0f : SwitchTint::partOff);
}
} // namespace phazite::ui
