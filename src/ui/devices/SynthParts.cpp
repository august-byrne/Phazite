// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SynthParts.h"
#include "DeviceTint.h"
#include "devices/dsp/Oscillators.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr float lowestGain = -36.0f, highestGain = 24.0f; // a filter's response, in decibels

    void paintFrame (juce::Graphics& g, const juce::Component& c)
    {
        graph::paintBackground (g, getPalette (c), c.getLocalBounds().toFloat());
    }
} // namespace

//==============================================================================
EnvelopeView::EnvelopeView (const juce::String& title, ParameterHandle a, ParameterHandle d, ParameterHandle s, ParameterHandle r,
                            std::function<float()> newestLevel)
    : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
      attack (std::move (a)),
      decay (std::move (d)),
      sustain (std::move (s)),
      release (std::move (r)),
      level (std::move (newestLevel))
{
    setTitle (title);
    setDescription ("The envelope's shape: its attack, decay, sustain and release, with a line at the newest note's level.");
}

void EnvelopeView::tick()
{
    if (const auto now = level != nullptr ? level() : 0.0f; ! juce::approximatelyEqual (now, shownLevel))
        shownLevel = now;

    repaint();
}

void EnvelopeView::paint (juce::Graphics& g)
{
    paintFrame (g, *this);
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getLocalBounds().toFloat().reduced (4.0f);

    // The stages side by side, a held sustain between the decay and the release, each at least a little wide.
    const auto a = std::max (5.0f, attack.getValue()), d = std::max (5.0f, decay.getValue()), r = std::max (5.0f, release.getValue());
    const auto hold = (a + d + r) * 0.4f;
    const auto total = a + d + hold + r;
    const auto x = [&area, total] (float time) { return area.getX() + time / total * area.getWidth(); };
    const auto y = [&area] (float value) { return area.getBottom() - value * area.getHeight(); };
    const auto held = juce::jlimit (0.0f, 1.0f, sustain.getValue());

    juce::Path shape;
    shape.startNewSubPath (x (0.0f), y (0.0f));
    shape.quadraticTo (x (a * 0.3f), y (0.95f), x (a), y (1.0f));
    shape.quadraticTo (x (a + d * 0.15f), y (held), x (a + d), y (held));
    shape.lineTo (x (a + d + hold), y (held));
    shape.quadraticTo (x (a + d + hold + r * 0.15f), y (0.0f), x (total), y (0.0f));

    auto filled = shape;
    filled.lineTo (x (0.0f), y (0.0f));
    filled.closeSubPath();
    g.setColour (tint.withAlpha (GraphTint::partCurve));
    g.fillPath (filled);
    g.setColour (tint);
    g.strokePath (shape, juce::PathStrokeType (GraphTint::curveWidth));

    // The newest note's level.
    if (shownLevel > 0.001f)
    {
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.drawHorizontalLine (juce::roundToInt (y (shownLevel)), area.getX(), area.getRight());
        g.setColour (p.text);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ area.getRight() - 3.0f, y (shownLevel) }));
    }
}

float getDrawnLfoValue (int shape, float phase)
{
    using devices::dsp::LfoShape;
    const auto lfoShape = (LfoShape) juce::jlimit (0, (int) LfoShape::random, shape);

    if (lfoShape == LfoShape::random)
        return std::sin (37.0f * std::floor (phase * 4.0f)) * 0.8f; // held values, a step at a time

    return devices::dsp::Lfo::getValue (lfoShape, phase);
}

//==============================================================================
LfoView::LfoView (const juce::String& title, std::function<float (float)> shape, std::function<float()> now)
    : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
      shapeAt (std::move (shape)),
      phase (std::move (now))
{
    setTitle (title);
    setDescription ("The LFO's wave, one cycle, with a dot where it is now.");
}

void LfoView::tick()
{
    shownPhase = phase != nullptr ? phase() : -1.0f;
    repaint();
}

void LfoView::paint (juce::Graphics& g)
{
    paintFrame (g, *this);
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getLocalBounds().toFloat().reduced (4.0f);
    const auto yFor = [&area] (float value) { return area.getCentreY() - juce::jlimit (-1.0f, 1.0f, value) * area.getHeight() * 0.45f; };

    g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
    g.drawHorizontalLine (juce::roundToInt (area.getCentreY()), area.getX(), area.getRight());

    const auto curve = graph::makeCurve (area, [&] (float x) { return yFor (shapeAt ((x - area.getX()) / area.getWidth())); }, 1.0f);
    g.setColour (tint);
    g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

    if (shownPhase >= 0.0f)
    {
        const juce::Point<float> dot { area.getX() + shownPhase * area.getWidth(), yFor (shapeAt (shownPhase)) };
        g.setColour (p.text);
        g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
    }
}

//==============================================================================
FilterView::FilterView (const juce::String& title, ParameterHandle c, ParameterHandle r, std::function<std::vector<float>()> read,
                        Response response, std::function<float()> nowPlaying)
    : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
      cutoff (std::move (c)),
      resonance (std::move (r)),
      readValues (std::move (read)),
      magnitude (std::move (response)),
      playing (std::move (nowPlaying))
{
    setTitle (title);
    setDescription ("The filter's response, and where the newest note plays it now. Drag the dot sideways for the cutoff and up "
                    "or down for the resonance.");
}

graph::FrequencyAxis FilterView::getFrequencies() const
{
    return { getLocalBounds().toFloat().reduced (2.0f) };
}

juce::Point<float> FilterView::getDotPosition() const
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    return { getFrequencies().xFor (cutoff.getValue()),
             area.getBottom() - juce::jlimit (0.0f, 1.0f, resonance.getValue()) * area.getHeight() };
}

void FilterView::tick()
{
    shownPlaying = playing != nullptr ? playing() : 0.0f;
    repaint();
}

void FilterView::paint (juce::Graphics& g)
{
    paintFrame (g, *this);
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const auto frequencies = getFrequencies();
    const graph::LevelAxis levels { area, lowestGain, highestGain };
    graph::paintFrequencyGrid (g, p, frequencies, false);

    const auto values = readValues();
    const auto curveAt = [&] (double at)
    {
        return graph::makeCurve (area,
                                 [&] (float x)
                                 {
                                     const auto gain = magnitude (values, at, frequencies.frequencyAt (x));
                                     return levels.yFor ((float) juce::Decibels::gainToDecibels (gain, (double) lowestGain - 1.0));
                                 });
    };

    // Where the newest note plays it, faint, then as set.
    if (shownPlaying > 0.0f)
    {
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.strokePath (curveAt (shownPlaying), juce::PathStrokeType (1.0f));
    }

    g.setColour (tint);
    g.strokePath (curveAt (cutoff.getValue()), juce::PathStrokeType (GraphTint::curveWidth));

    const auto dot = getDotPosition();
    g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
    graph::paintReadout (g, p, area, graph::formatFrequency (cutoff.getValue()), juce::Justification::topLeft);
}

void FilterView::mouseDown (const juce::MouseEvent& e)
{
    cutoff.beginGesture();
    resonance.beginGesture();
    dragTo (e.position);
}

void FilterView::mouseDrag (const juce::MouseEvent& e)
{
    dragTo (e.position);
}

void FilterView::mouseUp (const juce::MouseEvent&)
{
    resonance.endGesture();
    cutoff.endGesture();
}

void FilterView::dragTo (juce::Point<float> position)
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const auto range = cutoff.getRange();
    cutoff.setValue (juce::jlimit (range.start, range.end, (float) getFrequencies().frequencyAt (position.x)));
    resonance.setValue (juce::jlimit (0.0f, 1.0f, (area.getBottom() - position.y) / std::max (1.0f, area.getHeight())));
    repaint();
}
} // namespace phazite::ui
