// SPDX-License-Identifier: AGPL-3.0-or-later
// What the playlist and the piano roll share (docs/design.md, sections 6 and 7): a time line in beats with a
// ruler, zoom, scrolling and the playhead; a grid that positions snap to; and the tools. The commands for
// these act on the editor used last (addTimelineCommands()), so the same keys work in both.
#pragma once

#include "SmartTool.h"
#include "TimelineView.h"
#include "model/arrangement/Grid.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <optional>

namespace phazite::ui
{
class TimelineEditor;

//==============================================================================
// The bar numbers above an editor's time line. Clicking or dragging there moves the playhead.
// The ruler over a time line. Its lane along the top holds the loop, as Logic Pro's cycle strip does: a drag there
// loops what it covers, a drag on the loop's edges or middle changes or moves it, a click turns looping on or off and
// a double-click loops the whole song. Ctrl+drag anywhere makes a loop too, as FL Studio's does. Lower down, a click or
// a drag moves the playhead.
class TimelineRuler : public juce::Component
{
public:
    explicit TimelineRuler (TimelineEditor&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    juce::Rectangle<int> getLoopLane() const; // along the top

private:
    // What a press in the loop's lane does once it drags.
    enum class LoopDrag
    {
        none,
        create, // loops from where it began to the pointer
        moveStart,
        moveEnd,
        move
    };

    TimelineEditor& editor;
    LoopDrag loopDrag = LoopDrag::none;
    bool loopDragged = false; // past the threshold: not a click
    double loopAnchor = 0.0;  // the beat where the press was, snapped
    double loopStartAtPress = 0.0, loopEndAtPress = 0.0;

    void movePlayhead (const juce::MouseEvent&);
    double getBeatAt (const juce::MouseEvent&) const; // snapped, unless Alt is held
    LoopDrag getLoopDragAt (const juce::MouseEvent&) const;
    void dragLoop (const juce::MouseEvent&);
};

//==============================================================================
class TimelineEditor : public juce::Component,
                       private juce::ScrollBar::Listener,
                       private juce::ComponentListener,
                       private juce::Timer
{
public:
    static constexpr int toolbarHeight = 36, rulerHeight = 28, scrollBarSize = 12;

    // The editor keeps its grid in the project as that GridOwner's (loadGridSettings()).
    TimelineEditor (CommandBridge&, GridOwner);
    ~TimelineEditor() override;

    te::Edit* getEdit() const noexcept { return edit.get(); }
    CommandBridge& getCommands() const noexcept { return commands; }

    //==============================================================================
    // The time line. Positions are song beats, but the ruler and the grid count from the time origin: the
    // playlist's is the start of the song, and the piano roll's the start of its clip, so that bar 1 is where
    // the clip starts, as in FL Studio and Live. The view doesn't scroll back past it.
    const TimelineView& getView() const noexcept { return view; }
    te::BeatPosition getTimeOrigin() const noexcept { return timeOrigin; }
    void setViewStart (te::BeatPosition);
    void zoomAround (double x, double factor); // x from the left of the time line
    void zoom (double factor);                 // around the playhead if it is in view, otherwise the middle
    void zoomTo (te::BeatRange);
    void zoomToFit(); // shows getContentRange()
    void centreOnPlayhead();
    void movePlayheadTo (te::BeatPosition); // playback starts there from now on (phazite::movePlayhead())
    te::BeatPosition getPlayheadBeat() const;
    bool isPlayheadShowingLinkedClip() const; // where a linked clip plays elsewhere (getLinkedPlayheadBeat()), for tests

    // While playing, turn the page when the playhead reaches the right edge.
    bool isFollowingPlayback() const noexcept { return followPlayback; }
    void setFollowingPlayback (bool shouldFollow) noexcept { followPlayback = shouldFollow; }

    // Scrolling while something is dragged past an edge. Call autoScroll() on each mouse drag, with the
    // pointer's position and the area it scrolls (in the same coordinates): near or past an edge, the view
    // scrolls that way, faster the further out the pointer is. Sideways that is the time line; up and down,
    // scrollVerticallyBy(). Returns true if anything scrolled. A drag that may scroll calls
    // juce::Component::beginDragAutoRepeat() when it starts, so drags keep coming while the mouse is still.
    enum class ScrollDirections
    {
        sideways,
        upAndDown,
        both
    };

    bool autoScroll (juce::Point<int> position, juce::Rectangle<int> area, ScrollDirections = ScrollDirections::both);
    static constexpr int autoRepeatInterval = 16; // milliseconds, for beginDragAutoRepeat()

    //==============================================================================
    // The grid.
    const GridSettings& getGrid() const noexcept { return grid; }
    void setGrid (const GridSettings&);       // and keeps it in the project
    te::TimecodeSnapType getSnapType() const; // the grid's size at the current zoom
    double getGridStepInBeats() const;
    te::BeatPosition snap (te::BeatPosition, juce::ModifierKeys) const;     // to the nearest line; Alt places freely
    te::BeatPosition snapDown (te::BeatPosition, juce::ModifierKeys) const; // to the line at or before it
    void showGridMenu();

    //==============================================================================
    // The tools.
    Tool getTool() const noexcept { return tool; }
    virtual bool canUseTool (Tool) const { return true; }
    void setTool (Tool);

    // The tool commands. Tool keys spring back, as in Bitwig and Live: tapping a tool's key switches to the
    // tool, but holding the key while using the mouse keeps the tool only until the key is let go (or, if a
    // drag is still going on then, until it ends). Buttons and the palette switch for good.
    void pressTool (Tool);
    void releaseTool(); // the key that pressed it was let go

    //==============================================================================
    // What the commands for the selection do in this editor.
    virtual bool canNudgeSelection() const = 0;
    virtual void nudgeSelection (int direction) = 0; // by a step of the grid: earlier (-1) or later (1)
    virtual bool canQuantizeSelection() const = 0;
    virtual void quantizeSelection() = 0;

    // Puts the current shortcuts into the toolbar's tooltips; call after the keymap changed.
    virtual void refreshShortcutTexts();

    // Whether the time line's commands act on this editor: the one used last, while it is on the screen. AppShell
    // sets it; the active editor has a line in the accent color along its top.
    bool isActive() const { return active && isShowing(); }
    void setActive (bool);

    // Called when the mouse goes down anywhere in the editor, before its buttons run their commands, so that
    // they act on this editor. (JUCE reports focus changes later.)
    std::function<void()> onUsed;

    // A right-click on the ruler, at a place in the song: the loop's commands. The playlist offers the song's
    // sections there too.
    virtual void showRulerMenu (te::BeatPosition);

protected:
    // Fills the editor's background, with the toolbar's border and, while active, the accent line.
    void paintEditorBackground (juce::Graphics&);

    // Call from the derived class's setEdit(): resets the view and loads the grid the project keeps.
    void setTimelineEdit (te::Edit*);

    // Moves the time origin; the view stays where it is in the song, unless that is before the new origin.
    void setTimeOrigin (te::BeatPosition);

    // Where beats are drawn, in this component's coordinates: x starts from beat view.start at the left.
    virtual juce::Rectangle<int> getTimelineArea() const = 0;
    // What zoom to fit shows; the scroll bar covers it and some room after it.
    virtual te::BeatRange getContentRange() const = 0;
    // The view moved or zoomed: lay out and repaint what depends on it.
    virtual void timelineChanged() = 0;
    virtual void toolChanged() {}
    virtual void gridChanged() {}
    // The vertical scroll bar was dragged; it is the derived class's to set up and use.
    virtual void verticalScrollBarMoved (double /*newRangeStart*/) {}
    // Scrolls the content up (negative) or down by some pixels, for autoScroll(); returns true if it moved.
    virtual bool scrollVerticallyBy (int /*pixels*/) { return false; }
    // Where the playhead shows instead of the song's position, in song beats: the piano roll shows there where the
    // notes it edits are playing while a clip linked to its own plays elsewhere in the song. Nothing as a rule.
    virtual std::optional<double> getLinkedPlayheadBeat (double /*songBeat*/) const { return {}; }
    // What a tool does here, for its button's tooltip.
    virtual juce::String describeToolHere (Tool toolToDescribe) const { return describeTool (toolToDescribe); }

    // The toolbar's controls, which the derived class lays out (layOutToolbarControls() does the usual).
    juce::OwnedArray<IconButton> toolButtons; // one for each of allTools, in order; shown if canUseTool()
    IconButton snapButton { "Snap to grid", Icon::magnet };
    juce::TextButton gridButton;
    TimelineRuler ruler { *this };
    juce::ScrollBar horizontalScrollBar { false }, verticalScrollBar { true };

    // The room the vertical scroll bar takes: none while everything fits, when it hides (the editor is laid out again
    // when it shows or hides). The horizontal one always shows, as the time line goes on past the song.
    int getVerticalScrollBarWidth() const noexcept { return verticalScrollBar.isVisible() ? scrollBarSize : 0; }

    // Lays out the snap and grid buttons and the tools from the left of the toolbar; returns what is left.
    juce::Rectangle<int> layOutToolbarControls (juce::Rectangle<int> toolbar);

    // The mouse wheel over the time line. Each notch zooms by a step around x, or scrolls in time.
    void zoomByWheel (float x, float wheelDelta);
    void scrollByWheel (float wheelDelta);
    static double getWheelNotches (float wheelDelta);

    void updatePlayhead();
    void updateScrollBar();
    void updateGridControls();

private:
    struct Playhead;
    struct UseListener;

    CommandBridge& commands;
    const GridOwner gridOwner;
    te::SafeSelectable<te::Edit> edit;
    TimelineView view;
    te::BeatPosition timeOrigin;
    GridSettings grid;
    Tool tool = Tool::smart;
    std::optional<Tool> toolBeforeKey; // while a tool's key is down: the tool to spring back to
    bool usedWhileKeyDown = false;     // and whether the mouse was pressed meanwhile
    std::optional<Tool> toolAfterDrag; // the key was let go during a drag: the tool to go back to when it ends
    bool mouseIsDown = false;
    bool followPlayback = true, wasPlaying = false, active = false;
    bool layingOutForScrollBar = false, scrollBarChangedMeanwhile = false;
    double contentEnd = 0.0;         // in beats, as the scroll bar knows it
    te::BeatRange shownLoop;         // the loop the ruler shows (empty while playback doesn't loop), to repaint it on changes
    double lastAutoScrollTime = 0.0; // milliseconds (juce::Time::getMillisecondCounterHiRes())
    std::unique_ptr<Playhead> playhead;
    std::unique_ptr<UseListener> useListener;

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void componentVisibilityChanged (juce::Component&) override; // the vertical scroll bar showed or hid itself
    void timerCallback() override;                               // moves the playhead, and follows it while playing

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineEditor)
};

//==============================================================================
// Paints the grid's vertical lines over an area of an editor: bars, beats and the steps between them, as
// fine as the snapping unless the lines would be too close to see. area is in the editor's time line
// coordinates (x 0 is view.start), as the Graphics context is.
void paintGridLines (juce::Graphics&, const TimelineEditor&, juce::Rectangle<int> area);

// The commands for the editor used last (getActiveEditor() may return nullptr): zooming, following
// playback, the grid, the tools, and nudging and quantizing the selection.
void addTimelineCommands (CommandRegistry&, std::function<TimelineEditor*()> getActiveEditor);
} // namespace phazite::ui
