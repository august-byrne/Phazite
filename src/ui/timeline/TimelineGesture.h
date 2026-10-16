// SPDX-License-Identifier: AGPL-3.0-or-later
// What the playlist's and the piano roll's tools share while the mouse is down (roadmap phase A2): where it went down,
// when a press becomes a drag, the undo step of what the drag changes, and what a tool that acts on each thing the mouse
// passes over has done already. Each editor's gesture adds what its own tools need.
#pragma once

#include "TimelineView.h"
#include "model/commands/SongGesture.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
struct TimelineGesture
{
    // A press becomes a drag once the mouse moves this many pixels.
    static constexpr int dragThreshold = 4;
    static bool isPastDragThreshold (const juce::MouseEvent& e) { return e.getDistanceFromDragStart() >= dragThreshold; }

    // Where the mouse went down, where that is now: its beat, and its height moved by how far the content has scrolled
    // since. Positions above unscrolledHeight (the playlist's pinned tracks) don't scroll.
    juce::Point<int> getStart (const TimelineView&, int scrollYNow, int unscrolledHeight = 0) const;

    // Begins the undo step of what the gesture changes in the open project, unless it has one already: what a click does
    // and the drag that follows it are one step (a note added, then moved). The step lasts until the gesture ends, or
    // endStep().
    void beginStep (ProjectContext&, const juce::String& name);
    void endStep() { step = nullptr; }

    // For tools that act once on each thing the mouse passes over (Delete and Mute): true the first time for each
    // thing, which then counts as done.
    bool doOnce (const juce::ValueTree& thing);

    juce::Point<int> startPosition; // where the mouse went down, in the component it went down in
    BeatPosition startBeat;         // the beat under it then
    int startScrollY = 0;           // how far the content had scrolled then
    bool clickOnMouseUp = false;    // the click's selection waits for the mouse to go up without a drag
    BeatPosition nextPaintStart;    // Paint: where the next copy goes

private:
    std::unique_ptr<SongGesture> step;
    std::vector<juce::ValueTree> done;
};
} // namespace phazite::ui
