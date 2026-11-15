// SPDX-License-Identifier: AGPL-3.0-or-later
// What a device's graph shows under the pointer of what a drag there moves (the owner, 2026-11-10: "it should be obvious that
// you are able to do this gesture"), as the EQ's dots show it: the pointer says how the drag moves (sideways, up and down, or
// both ways), and the handle it takes lights up, ringed in the text colour (GraphTint::litGap, litWidth).
//
// GraphDot is the dot of a graph whose two values it moves, one across and one up (a filter's cutoff and resonance, a
// chorus's delay and depth): over the dot, the grabbing hand, and the dot lit, as it stays while a press drags it; elsewhere,
// the crosshair, where a press would put it. Given its values (setDrag()), it drags them too, as every graph's handles drag
// (graph::HandleDrag): a press on the dot takes it from where its values set by hand put it, changing nothing, and the drag
// moves it as far as the pointer goes; a press elsewhere puts it under the pointer; both values move as one undo step, and
// Escape takes the drag back. It hears the graph's mouse events after the graph itself.
#pragma once

#include "DeviceGraph.h"
#include "ui/theme/DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace phazite::ui
{
class GraphDot : private juce::MouseListener
{
public:
    // The graph, and where it draws the dot now, in the graph's own coordinates.
    GraphDot (juce::Component& graph, std::function<juce::Point<float>()> getCentre);
    ~GraphDot() override;

    // One of the values the dot shows: its parameter's handle (the graph's, which must outlive the dot), where a value is
    // along the graph (placeOf: an x for the value across, a y for the one up, in the graph's coordinates), and the value at a
    // place there (valueAt).
    struct Value
    {
        ParameterHandle* handle = nullptr;
        std::function<float (float value)> placeOf;
        std::function<float (float place)> valueAt;
    };

    // The values a drag moves, the one across first (the undo step is named after it); moved: after each change a drag makes,
    // and after Escape takes one back, for the graph to show it.
    void setDrag (Value across, Value up, std::function<void()> moved);

    // The dot's drag, for the graph's wheels (graph::WheelGesture), which a press ends and which turn nothing during a drag.
    graph::HandleDrag& getDrag() noexcept { return drag; }

    // Whether the dot is lit now, for a graph that draws a dot of its own (as the synths' filter views draw a smaller one).
    bool isLit() const noexcept { return lit; }

    // The dot, filled in the tint and ringed in the text colour; lit, as paintLitRing() rings it.
    void paint (juce::Graphics&, const Palette&, juce::Colour tint) const;

private:
    juce::Component& graph;
    const std::function<juce::Point<float>()> getCentre;
    Value across, up;
    std::function<void()> moved;
    graph::HandleDrag drag { graph };
    juce::Point<float> startPlace;       // where the drag moves the dot from: its values' place as the press took it
    std::array<float, 2> startValues {}; // and its values there
    bool lit = false;

    bool isOnDot (juce::Point<float>) const;
    void show (bool shouldBeLit);
    void moveBy (juce::Point<float> offset); // from the start, as far as the pointer moved

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    JUCE_DECLARE_NON_COPYABLE (GraphDot)
};

// A dot of a graph lit up, under the pointer or while dragged: ringed in the text colour from its edge out, as the EQ's; and
// a bar with rounded corners, the same.
void paintLitRing (juce::Graphics&, const Palette&, juce::Point<float> centre, float radius);
void paintLitRing (juce::Graphics&, const Palette&, juce::Rectangle<float> bar, float cornerSize);
} // namespace phazite::ui
