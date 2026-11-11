// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GraphDot.h"

namespace phazite::ui
{
namespace
{
    juce::Rectangle<float> makeCircle (juce::Point<float> centre, float radius)
    {
        return juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (centre);
    }
} // namespace

GraphDot::GraphDot (juce::Component& g, std::function<juce::Point<float>()> centre)
    : graph (g),
      getCentre (std::move (centre))
{
    graph.setMouseCursor (juce::MouseCursor::CrosshairCursor);
    graph.addMouseListener (this, false);
}

GraphDot::~GraphDot()
{
    graph.removeMouseListener (this);
}

void GraphDot::paint (juce::Graphics& g, const Palette& p, juce::Colour tint) const
{
    const auto centre = getCentre();
    g.setColour (tint);
    g.fillEllipse (makeCircle (centre, GraphTint::dotRadius));

    if (lit)
    {
        paintLitRing (g, p, centre, GraphTint::dotRadius);
        return;
    }

    g.setColour (p.text);
    g.drawEllipse (makeCircle (centre, GraphTint::dotRadius), GraphTint::handleOutline);
}

bool GraphDot::isOnDot (juce::Point<float> position) const
{
    return position.getDistanceFrom (getCentre()) <= GraphTint::grabDistance;
}

void GraphDot::show (bool shouldBeLit)
{
    graph.setMouseCursor (shouldBeLit ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);

    if (shouldBeLit != lit)
    {
        lit = shouldBeLit;
        graph.repaint();
    }
}

void GraphDot::mouseEnter (const juce::MouseEvent& e)
{
    show (isOnDot (e.position));
}

void GraphDot::mouseMove (const juce::MouseEvent& e)
{
    show (isOnDot (e.position));
}

// The graph's own press has put the dot under the pointer, as near as it goes, and the drag takes it along.
void GraphDot::mouseDown (const juce::MouseEvent&)
{
    show (true);
}

void GraphDot::mouseUp (const juce::MouseEvent& e)
{
    show (isOnDot (e.position));
}

void GraphDot::mouseExit (const juce::MouseEvent&)
{
    show (false);
}

void paintLitRing (juce::Graphics& g, const Palette& p, juce::Point<float> centre, float radius)
{
    g.setColour (p.text);
    g.drawEllipse (makeCircle (centre, radius).expanded (GraphTint::litGap), GraphTint::litWidth);
}

void paintLitRing (juce::Graphics& g, const Palette& p, juce::Rectangle<float> bar, float cornerSize)
{
    g.setColour (p.text);
    g.drawRoundedRectangle (bar.expanded (GraphTint::litGap), cornerSize + GraphTint::litGap, GraphTint::litWidth);
}
} // namespace phazite::ui
