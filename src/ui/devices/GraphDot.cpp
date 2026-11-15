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
    drag.onCancelled = [this]
    {
        if (moved != nullptr)
            moved();
    };
}

GraphDot::~GraphDot()
{
    graph.removeMouseListener (this);
}

void GraphDot::setDrag (Value newAcross, Value newUp, std::function<void()> onMoved)
{
    across = std::move (newAcross);
    up = std::move (newUp);
    moved = std::move (onMoved);
}

// Each value from its start, at the place the pointer has moved its own axis to; one the pointer hasn't moved along is its start,
// exactly (a round trip through the pixels may not give it back).
void GraphDot::moveBy (juce::Point<float> offset)
{
    const auto moveAxis = [this] (size_t index, const Value& value, float from, float by)
    { drag.set (index, juce::exactlyEqual (by, 0.0f) ? startValues[index] : value.valueAt (from + by)); };

    moveAxis (0, across, startPlace.x, offset.x);
    moveAxis (1, up, startPlace.y, offset.y);

    if (moved != nullptr)
        moved();
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

// On the dot, the press takes it from where its values set by hand put it, and changes nothing; elsewhere, it puts the dot
// under the pointer, as near as it goes. The drag takes it along.
void GraphDot::mouseDown (const juce::MouseEvent& e)
{
    if (across.handle != nullptr && up.handle != nullptr && ! e.mods.isPopupMenu())
    {
        const auto onDot = isOnDot (e.position);
        drag.take ({ across.handle, up.handle });

        if (onDot)
        {
            startValues = { drag.getStart (0), drag.getStart (1) };
            startPlace = { across.placeOf (startValues[0]), up.placeOf (startValues[1]) };
        }
        else
        {
            startPlace = e.position;
            startValues = { across.valueAt (startPlace.x), up.valueAt (startPlace.y) };
            moveBy ({});
        }
    }

    show (true);
}

void GraphDot::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.isTaken())
        moveBy (e.getOffsetFromDragStart().toFloat());
}

void GraphDot::mouseUp (const juce::MouseEvent& e)
{
    drag.end();
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
