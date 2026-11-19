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
    graph.setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    graph.addMouseListener (this, false);
    drag.onCancelled = [this]
    {
        if (moved != nullptr)
            moved();
    };
    drag.findHandle = [this] (juce::Point<float>) { return findHandle(); };
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

void GraphDot::setWheel (std::function<ParameterHandle*()> turned)
{
    wheel = std::move (turned);
}

// The graph's one handle, anywhere on it: both values, and what the wheel turns.
std::optional<graph::Handle> GraphDot::findHandle() const
{
    if (across.handle == nullptr || up.handle == nullptr)
        return std::nullopt;

    return graph::Handle ({ across.handle, up.handle }, getCentre(), wheel != nullptr ? wheel() : nullptr);
}

// Each value from its start, at the place the pointer has moved its own axis to; one the pointer hasn't moved along is its start,
// exactly (a round trip through the pixels may not give it back).
void GraphDot::moveBy (juce::Point<float> offset)
{
    const auto moveAxis = [this] (size_t index, const Value& value, float from, float by)
    { drag.set (index, juce::exactlyEqual (by, 0.0f) ? drag.getStart (index) : value.valueAt (from + by)); };

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

// Anywhere on the graph, the press takes the dot from where its values set by hand put it, and changes nothing: a press beside
// the dot doesn't move it there. The drag takes it along. A right-click is the drag's (its menu).
void GraphDot::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    if (across.handle != nullptr && up.handle != nullptr)
    {
        drag.take ({ across.handle, up.handle });
        startPlace = { across.placeOf (drag.getStart (0)), up.placeOf (drag.getStart (1)) };
    }

    show (true);
}

void GraphDot::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.isTaken())
        moveBy (drag.getMoved (e));
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
