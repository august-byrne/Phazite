// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LevelHistoryView.h"
#include "DeviceTint.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int numberedDips = 3;     // the deepest reductions shown with their decibels
    constexpr float smallestDip = 0.5f; // dB: shallower ones aren't numbered
} // namespace

LevelHistoryView::LevelHistoryView (const juce::String& title, float lowestLevel, float range, int ticks)
    : lowest (lowestLevel),
      reductionRange (range),
      inputs (ticks, lowestLevel),
      outputs (ticks, lowestLevel),
      reductions (ticks, 0.0f)
{
    setTitle (title);
    setDescription ("How loud the sound came in (pale) and went out over the last seconds, and how far it was turned down (red, "
                    "from the top).");
}

void LevelHistoryView::push (float input, float output, float reduction)
{
    inputs.push (input);
    outputs.push (output);
    reductions.push (reduction);
    repaint();
}

void LevelHistoryView::setLine (ParameterHandle* handle, const juce::String& name)
{
    line = handle;
    lineName = name;

    if (line != nullptr)
        setDescription (getDescription() + " Drag the " + name.toLowerCase() + "'s line up or down to move it.");

    repaint();
}

graph::LevelAxis LevelHistoryView::getLevels() const
{
    return { getLocalBounds().toFloat().reduced (1.0f), lowest, 0.0f };
}

float LevelHistoryView::getYForLevel (float decibels) const
{
    return getLevels().yFor (decibels);
}

void LevelHistoryView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getLocalBounds().toFloat();
    const auto levels = getLevels();
    graph::paintBackground (g, p, area);
    graph::paintLevelGrid (g, p, levels, 12.0f, true);

    const auto yFor = [&levels, this] (float decibels) { return levels.yFor (juce::jlimit (lowest, 0.0f, decibels)); };
    const auto inner = levels.area;

    // In, pale; out, in the device's colour, over it.
    g.setColour (p.textMuted.withAlpha (GraphTint::spectrumFill * 2.0f));
    g.fillPath (inputs.makeArea (inner, yFor, inner.getBottom()));
    g.setColour (tint.withAlpha (GraphTint::partCurve * 2.0f));
    g.fillPath (outputs.makeArea (inner, yFor, inner.getBottom()));
    g.setColour (tint);
    g.strokePath (outputs.makeLine (inner, yFor), juce::PathStrokeType (1.0f));

    // Turned down: hanging from the top, as deep as it went.
    const auto reductionY = [&inner, this] (float reduction)
    { return inner.getY() + juce::jlimit (0.0f, 1.0f, reduction / reductionRange) * inner.getHeight() * 0.5f; };
    g.setColour (p.danger.withAlpha (GraphTint::partCurve * 2.0f));
    g.fillPath (reductions.makeArea (inner, reductionY, inner.getY()));
    g.setColour (p.danger);
    g.strokePath (reductions.makeLine (inner, reductionY), juce::PathStrokeType (1.0f));

    // The deepest dips, numbered, each the deepest of its stretch of the history.
    const auto stretch = std::max (1, reductions.size() / numberedDips);

    for (int start = 0; start + stretch <= reductions.size(); start += stretch)
    {
        auto deepest = start;

        for (int i = start; i < start + stretch; ++i)
            if (reductions.get (i) > reductions.get (deepest))
                deepest = i;

        if (const auto dip = reductions.get (deepest); dip >= smallestDip)
        {
            const auto x = inner.getX() + (float) deepest / (float) (reductions.size() - 1) * inner.getWidth();
            graph::paintReadout (g, p, juce::Rectangle<float> (x - 30.0f, reductionY (dip), 60.0f, TextSize::small + 6.0f),
                                 "-" + juce::String (dip, 1), juce::Justification::centredTop);
        }
    }

    // The line across, with its name and level.
    if (line != nullptr)
    {
        const auto y = levels.yFor (line->getValue());
        g.setColour (p.text.withAlpha (dragging ? 1.0f : GraphTint::marker * 1.5f));
        g.drawLine (inner.getX(), y, inner.getRight(), y, dragging ? 2.0f : 1.0f);
        graph::paintReadout (g, p,
                             juce::Rectangle<float> (inner.getX(), y - TextSize::small - 4.0f, inner.getWidth(), TextSize::small + 4.0f),
                             lineName + " " + graph::formatDecibels (line->getValue()), juce::Justification::bottomRight);
    }
}

void LevelHistoryView::mouseDown (const juce::MouseEvent&)
{
    if (line == nullptr)
        return;

    dragging = true;
    line->beginGesture();
    startValue = line->getValue();
    repaint();
}

void LevelHistoryView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const auto levels = getLevels();
    const auto perPixel = (levels.high - levels.low) / std::max (1.0f, levels.area.getHeight());
    const auto range = line->getRange();
    line->setValue (
        range.snapToLegalValue (juce::jlimit (range.start, range.end, startValue - (float) e.getDistanceFromDragStartY() * perPixel)));
    repaint();
}

void LevelHistoryView::mouseUp (const juce::MouseEvent&)
{
    if (dragging && line != nullptr)
        line->endGesture();

    dragging = false;
    repaint();
}

void LevelHistoryView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (line != nullptr)
        if (const auto initial = line->getDefaultValue())
            line->setValue (*initial);
}
} // namespace phazite::ui
