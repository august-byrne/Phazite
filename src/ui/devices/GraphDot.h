// SPDX-License-Identifier: AGPL-3.0-or-later
// What a device's graph shows under the pointer of what a drag there moves (the owner, 2026-11-10: "it should be obvious that
// you are able to do this gesture"), as the EQ's dots show it: the pointer says how the drag moves (sideways, up and down, or
// both ways), and the handle it takes lights up, ringed in the text colour (GraphTint::litGap, litWidth).
//
// GraphDot is the dot of a graph whose two values it moves, one across and one up (a filter's cutoff and resonance, a
// chorus's delay and depth), which a press anywhere on the graph puts under the pointer: over the dot, the grabbing hand, and
// the dot lit, as it stays while a press drags it; elsewhere, the crosshair, where a press would put it. It hears the graph's
// mouse events after the graph itself, so that the graph's own handlers have moved the dot when it looks.
#pragma once

#include "ui/theme/DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace phazite::ui
{
class GraphDot : private juce::MouseListener
{
public:
    // The graph, and where it draws the dot now, in the graph's own coordinates.
    GraphDot (juce::Component& graph, std::function<juce::Point<float>()> getCentre);
    ~GraphDot() override;

    // Whether the dot is lit now, for a graph that draws a dot of its own (as the synths' filter views draw a smaller one).
    bool isLit() const noexcept { return lit; }

    // The dot, filled in the tint and ringed in the text colour; lit, as paintLitRing() rings it.
    void paint (juce::Graphics&, const Palette&, juce::Colour tint) const;

private:
    juce::Component& graph;
    const std::function<juce::Point<float>()> getCentre;
    bool lit = false;

    bool isOnDot (juce::Point<float>) const;
    void show (bool shouldBeLit);

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    JUCE_DECLARE_NON_COPYABLE (GraphDot)
};

// A dot of a graph lit up, under the pointer or while dragged: ringed in the text colour from its edge out, as the EQ's; and
// a bar with rounded corners, the same.
void paintLitRing (juce::Graphics&, const Palette&, juce::Point<float> centre, float radius);
void paintLitRing (juce::Graphics&, const Palette&, juce::Rectangle<float> bar, float cornerSize);
} // namespace phazite::ui
