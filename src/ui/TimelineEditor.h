// SPDX-License-Identifier: AGPL-3.0-or-later
// What the playlist and the piano roll share (docs/design.md, sections 6 and 7): a time line in beats with a
// ruler, zoom, scrolling and the playhead; a grid that positions snap to; and the tools. The commands for
// these act on the editor used last (addTimelineCommands()), so the same keys work in both.
#pragma once

#include "Icons.h"
#include "model/Commands.h"
#include "model/SmartTool.h"
#include "model/Timeline.h"

namespace phazite::ui
{
class TimelineEditor;

//==============================================================================
// The bar numbers above an editor's time line. Clicking or dragging there moves the playhead.
class TimelineRuler : public juce::Component
{
public:
    explicit TimelineRuler (TimelineEditor&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    TimelineEditor& editor;

    void movePlayhead (const juce::MouseEvent&);
};

//==============================================================================
class TimelineEditor : public juce::Component,
                       private juce::ScrollBar::Listener,
                       private juce::Timer
{
public:
    static constexpr int toolbarHeight = 36, rulerHeight = 28, scrollBarSize = 12;

    // The editor keeps its grid in the project's Phazite state, in a child of type gridStateType, or in
    // the state itself if that is invalid.
    TimelineEditor (CommandRegistry&, const juce::Identifier& gridStateType);
    ~TimelineEditor() override;

    te::Edit* getEdit() const noexcept { return edit.get(); }
    CommandRegistry& getCommands() const noexcept { return commands; }

    //==============================================================================
    // The time line.
    const TimelineView& getView() const noexcept { return view; }
    void setViewStart (te::BeatPosition);
    void zoomAround (double x, double factor); // x from the left of the time line
    void zoom (double factor);                 // around the playhead if it is in view, otherwise the middle
    void zoomTo (te::BeatRange);
    void zoomToFit(); // shows getContentRange()
    void centreOnPlayhead();
    void movePlayheadTo (te::BeatPosition);
    te::BeatPosition getPlayheadBeat() const;

    // While playing, turn the page when the playhead reaches the right edge.
    bool isFollowingPlayback() const noexcept { return followPlayback; }
    void setFollowingPlayback (bool shouldFollow) noexcept { followPlayback = shouldFollow; }

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
    void mouseDown (const juce::MouseEvent&) override;

protected:
    // Fills the editor's background, with the toolbar's border and, while active, the accent line.
    void paintEditorBackground (juce::Graphics&);

    // Call from the derived class's setEdit(): resets the view and loads the grid the project keeps.
    void setTimelineEdit (te::Edit*);

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
    // What a tool does here, for its button's tooltip.
    virtual juce::String describeToolHere (Tool toolToDescribe) const { return describeTool (toolToDescribe); }

    // The toolbar's controls, which the derived class lays out (layOutToolbarControls() does the usual).
    juce::OwnedArray<IconButton> toolButtons; // one for each of allTools, in order; shown if canUseTool()
    IconButton snapButton { "Snap to grid", Icon::magnet };
    juce::TextButton gridButton;
    TimelineRuler ruler { *this };
    juce::ScrollBar horizontalScrollBar { false }, verticalScrollBar { true };

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

    CommandRegistry& commands;
    const juce::Identifier gridStateType;
    te::SafeSelectable<te::Edit> edit;
    TimelineView view;
    GridSettings grid;
    Tool tool = Tool::smart;
    bool followPlayback = true, wasPlaying = false, active = false;
    double contentEnd = 0.0; // in beats, as the scroll bar knows it
    std::unique_ptr<Playhead> playhead;

    juce::ValueTree getGridState (bool createIfMissing) const;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override; // moves the playhead, and follows it while playing

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
