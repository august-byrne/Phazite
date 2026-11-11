// SPDX-License-Identifier: AGPL-3.0-or-later
// What the playlist and the piano roll share (docs/design.md, sections 6 and 7): a time line in beats with a
// ruler, zoom, scrolling and the playhead; a grid that positions snap to; and the tools. The commands for
// these act on the editor used last (addTimelineCommands()), so the same keys work in both.
#pragma once

#include "FrameLog.h"
#include "SmartTool.h"
#include "TimelineView.h"
#include "model/arrangement/TimelineModel.h"
#include "model/project/ProjectContext.h"
#include "ui/components/ScrollBarFade.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <optional>

namespace phazite::ui
{
class TimelineEditor;

//==============================================================================
// The ruler over a time line: the bar numbers, each by a tick from its foot, and shorter ticks for the other bars and the
// beats where they are far enough apart to read, nothing finer (the owner, 2026-11-06: "less busy"; the grid under the
// ruler shows the rest). Its lane along the top holds the loop, as Logic Pro's cycle strip does, washed under the pointer:
// a drag there loops what it covers, a drag on the loop's edges or middle changes or moves it, a click turns looping on or
// off and a double-click loops the whole song. Ctrl+drag anywhere makes a loop too, as FL Studio's does. Lower down, a
// press takes the playhead there and a drag takes it along; the song is silent meanwhile and plays on from where it is let
// go, if it played.
class TimelineRuler : public juce::Component
{
public:
    explicit TimelineRuler (TimelineEditor&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    juce::Rectangle<int> getLoopLane() const;                   // along the top
    bool isLoopLaneLit() const noexcept { return laneHovered; } // under the pointer (for tests)

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
    bool loopDragged = false;      // past the threshold: not a click
    bool draggingPlayhead = false; // a press below the loop's lane, until it is let go
    bool pausedForDrag = false;    // and whether it paused playback, to play on at its end
    bool laneHovered = false;      // the loop's lane, under the pointer
    double loopAnchor = 0.0;       // the beat where the press was, snapped
    double loopStartAtPress = 0.0, loopEndAtPress = 0.0;

    void movePlayhead (const juce::MouseEvent&);
    double getBeatAt (const juce::MouseEvent&) const; // snapped, unless Alt is held
    LoopDrag getLoopDragAt (const juce::MouseEvent&) const;
    void dragLoop (const juce::MouseEvent&);
};

//==============================================================================
// The window a part of an editor that draws the time line shows through (TimelineEditor::addTimelinePart()): on whole pixels,
// untransformed, where the part shows, and clipping it there. The part fills it from its left and is wider by as much as
// scrolling continuously moves it left (TimelineEditor::getSubPixel(), less than the view's step, a pixel at 100 %), so that
// it still covers all of it. JUCE workaround W11 (docs/juce-workarounds.md).
class TimelinePartWindow : public juce::Component,
                           private juce::ComponentListener
{
public:
    explicit TimelinePartWindow (juce::Component& part);
    ~TimelinePartWindow() override;

    juce::Component* getPart() const noexcept { return part.getComponent(); }
    void setOverhang (int pixels); // how much wider than the window the part is (TimelineEditor::getViewStep())
    void resized() override;

    // Opaque while its part shows and is (the part covers it whole, at any part of a pixel), so that what is behind needn't
    // paint at each frame the part moves by one; the part paints it all.
    void paint (juce::Graphics&) override {}

private:
    juce::Component::SafePointer<juce::Component> part; // which goes before its window, as the editor's derived class's
    int overhang = 1;

    void componentVisibilityChanged (juce::Component&) override;
    void componentBeingDeleted (juce::Component&) override;
};

// What of the time line shows around a component inside one of an editor's time line parts (a clip in the playlist's lanes),
// in that component's own coordinates: its part's window, which scrolling continuously moves the part a part of a pixel
// under. What stays in view as the song scrolls past the view's edges (a clip's name) goes in it, so that it stays put on the
// screen's pixels; in the part's own bounds it would move with the part, back and forth at every frame by up to a pixel. Its
// own bounds, if it is in none.
juce::Rectangle<float> getShownTimeline (const juce::Component&);

//==============================================================================
class TimelineEditor : public juce::Component,
                       private juce::ScrollBar::Listener,
                       private juce::ComponentListener,
                       private juce::Timer
{
public:
    static constexpr int toolbarHeight = 36, rulerHeight = 28, scrollBarSize = 12;

    // The editor keeps its grid in the project as that GridOwner's (loadGridSettings()).
    TimelineEditor (CommandBridge&, ProjectContext&, GridOwner);
    ~TimelineEditor() override;

    ProjectContext& getContext() const noexcept { return timelineContext; }
    bool hasProject() const noexcept { return timelineContext.hasProject(); }
    CommandBridge& getCommands() const noexcept { return commands; }

    //==============================================================================
    // The time line. Positions are song beats, but the ruler and the grid count from the time origin: the
    // playlist's is the start of the song, and the piano roll's the start of its clip, so that bar 1 is where
    // the clip starts, as in FL Studio and Live. The view doesn't scroll back past it. It starts on the whole pixel of the
    // song nearest the beat it is given, at its zoom (the origin's too, which can be up to half a pixel before the origin),
    // so that everything it shows moves together by whole pixels as it scrolls: started between pixels, each line and edge
    // took the column its own place rounded to, and they stepped at different moments as the view moved by parts of a pixel
    // (dragging past an edge, a trackpad, turning pages), lines' spacing and clips' widths changing by a pixel as they went.
    const TimelineView& getView() const noexcept { return view; }
    BeatPosition getTimeOrigin() const noexcept { return timeOrigin; }
    void setViewStart (BeatPosition);
    void zoomAround (double x, double factor); // x from the left of the time line
    void zoom (double factor);                 // around the playhead if it is in view, otherwise the middle
    void zoomTo (BeatRange);
    void zoomToFit(); // shows getContentRange()
    void centreOnPlayhead();

    // Whether the view stays where the editor puts it (zoomTo()): zooming, scrolling and centring by the user, and turning
    // the page while playing, leave it. The audio clip editor's shows its clip's whole audio, always.
    virtual bool isViewFixed() const { return false; }
    void movePlayheadTo (BeatPosition); // playback starts there from now on (phazite::movePlayhead())
    BeatPosition getPlayheadBeat() const;
    BeatPosition getShownPlayheadBeat() const; // where its line shows: while playing, the beat heard at the frame shown
    bool isPlayheadShowingLinkedClip() const;  // where a linked clip plays elsewhere (getLinkedPlayheadBeat()), for tests

    // A frame of the display, at its vertical blank's time (seconds by juce::Time::getMillisecondCounterHiRes()): while the
    // song plays where the editor shows, the playhead moves to the beat heard then (getHeardPlayheadBeat()) and the frame is
    // logged (FrameLog.h); once it stops, the playhead goes where playback went back to, and the log is written. The
    // display's own frames call it from the moment a project shows (a juce::VBlankAttachment); tests call it with times of
    // their own. JUCE workaround W27 (docs/juce-workarounds.md), for the frames' clock.
    void showFrame (double seconds);

    // Where the playhead's line is drawn, in pixels from the time line's left: between pixels, so that it moves evenly, but on
    // a whole pixel while the view scrolls under it (for tests).
    double getPlayheadX() const noexcept { return playheadX; }

    // Scrolling continuously, how far the song has moved past the view's whole step (getViewStep(), a pixel at 100 %), from 0
    // up to the step, by which the editor's parts that draw the time line (addTimelinePart()) are moved left inside their
    // windows, so that the song glides under the playhead rather than stepping a pixel at a time (docs/performance-audit.md,
    // section 16, option 15); 0 otherwise (for tests).
    float getSubPixel() const noexcept { return subPixel; }

    // The whole pixels the view's start goes by (setViewStart()): one where the display's scale makes each of the editor's
    // pixels a whole number of the screen's (100 %, 200 %), and otherwise the fewest that do (two at 150 %, four at 125 % and
    // 175 %), so that what is drawn on the screen's own pixels (TimelineView::beatToEdge()) is sharp in every view, as what is
    // drawn on the editor's columns is. Started on one of the editor's pixels, notes' edges came out half a screen pixel off in
    // every other view at 150 % (known bug 17 until 2026-11-11). Scrolling continuously moves the parts by less than this.
    int getViewStep() const noexcept { return viewStep; }

    // While playing, turn the page when the playhead reaches the right edge; or, scrolling continuously (View > Scroll
    // Continuously, which the app keeps in its settings), keep the playhead in the middle and scroll the song under it, as
    // Logic Pro's Scroll in Play and Cubase's Stationary Cursor do. Scrolling the view sideways while the song plays pauses
    // either, as in Ableton Live, until playback starts again, the playhead is moved or the view is centred on it
    // (centreOnPlayhead()).
    bool isFollowingPlayback() const noexcept { return followPlayback; }
    void setFollowingPlayback (bool shouldFollow) noexcept;
    bool isFollowingPaused() const noexcept { return followPaused; }
    bool isScrollingContinuously() const noexcept { return scrollContinuously; }
    void setScrollingContinuously (bool shouldScroll) noexcept { scrollContinuously = shouldScroll; }

    // Scrolling while something is dragged past an edge. Call autoScroll() on each mouse drag, with the drag
    // and the area it scrolls, in the coordinates of the drag's position (juce::MouseEvent::getEventRelativeTo()):
    // near or past an edge, the view scrolls that way, faster the further out the pointer is, by the time since the
    // drag before (the events' own times), however often the mouse reports. Sideways that is the time line; up and
    // down, scrollVerticallyBy(). Returns true if anything scrolled. A drag that may scroll calls
    // juce::Component::beginDragAutoRepeat() when it starts, so drags keep coming while the mouse is still.
    enum class ScrollDirections
    {
        sideways,
        upAndDown,
        both
    };

    bool autoScroll (const juce::MouseEvent&, juce::Rectangle<int> area, ScrollDirections = ScrollDirections::both);
    static constexpr int autoRepeatInterval = 16; // milliseconds, for beginDragAutoRepeat()

    //==============================================================================
    // The grid.
    const GridSettings& getGrid() const noexcept { return grid; }
    void setGrid (const GridSettings&); // and keeps it in the project
    GridStep getGridStep() const;       // the grid's size at the current zoom
    double getGridStepInBeats() const;
    BeatPosition snap (BeatPosition, juce::ModifierKeys) const;     // to the nearest line; Alt places freely
    BeatPosition snapDown (BeatPosition, juce::ModifierKeys) const; // to the line at or before it
    void showGridMenu();

    //==============================================================================
    // The tools.
    Tool getTool() const noexcept { return tool; }
    virtual bool canUseTool (Tool) const { return true; }
    void setTool (Tool);

    // Whether a tool's key and command pick the Smart Tool here instead, the tool's button hidden: in the piano roll the Draw
    // Tool would do just what the Smart Tool does (one way to do one thing).
    virtual bool usesSmartToolFor (Tool) const { return false; }

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

    // The Quantize command's name here, naming what it uses beyond the grid (the piano roll's strength and swing), or
    // nothing for the plain name.
    virtual juce::String getQuantizeName() const { return {}; }

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
    virtual void showRulerMenu (BeatPosition);

protected:
    // Fills the editor's background, with the toolbar's border and, while active, the accent line.
    void paintEditorBackground (juce::Graphics&);

    // Call when another project opens, or none (the derived class's projectChanged()): resets the view and loads the grid
    // the project keeps.
    void showProject();

    // Moves the time origin; the view stays where it is in the song, unless that is before the new origin.
    void setTimeOrigin (BeatPosition);

    // Where the view starts when it starts at the time origin: the whole pixel of the song nearest it (setViewStart()). A view
    // kept so many pixels from the origin counts them from here.
    BeatPosition getEarliestStart() const;

    // Where beats are drawn, in this component's coordinates: x starts from beat view.start at the left.
    virtual juce::Rectangle<int> getTimelineArea() const = 0;

    // The parts that draw the time line by the view: the ruler, and those the derived class adds, each once, from its
    // constructor, in the order the editor's children are to be drawn in. Each shows through a window of its own
    // (TimelinePartWindow), which the derived class places where the part is to show (placeTimelinePart()), and the part, a
    // step of the view wider (getViewStep(), a pixel at 100 %), fills it. Scrolling continuously moves the parts left by less
    // than a step (getSubPixel()) inside their windows, so that their edges stay on whole pixels: moved by themselves, a part's
    // edges fell between pixels, and the
    // renderer, rounding them out to whole ones, let the part paint over the column beside it at every frame that moved it one
    // way and not the other (the owner, 2026-11-11: the track headers' edge flickered while the playlist followed the playhead).
    // A part is shown or hidden as any child is.
    void addTimelinePart (juce::Component&);
    void placeTimelinePart (juce::Component&, juce::Rectangle<int> area);    // where it shows, in this component
    juce::Rectangle<int> getTimelinePartArea (const juce::Component&) const; // where it shows
    // What zoom to fit shows; the scroll bar covers it and some room after it.
    virtual BeatRange getContentRange() const = 0;
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
    // The playhead's line was placed again (at each frame of the display while the song plays): for what follows it, as the
    // playlist's recordings grow with it.
    virtual void playheadShown() {}
    // A frame of the display is about to be painted (at each one, playing or not, before showFrame() moves the playhead): for
    // what must be up to date in it that the editor would otherwise hear of a message later, after the frame.
    virtual void frameComing() {}
    // What a tool does here, for its button's tooltip.
    virtual juce::String describeToolHere (Tool toolToDescribe) const { return describeTool (toolToDescribe); }

    // The toolbar's controls, which the derived class lays out (layOutToolbarControls() does the usual).
    juce::OwnedArray<IconButton> toolButtons; // one for each of allTools, in order; shown if canUseTool()
    IconButton snapButton { "Snap to grid", Icon::magnet };
    juce::TextButton gridButton;
    TimelineRuler ruler { *this };
    juce::ScrollBar horizontalScrollBar { false }, verticalScrollBar { true };
    ScrollBarFade verticalFade { verticalScrollBar, *this }; // the rows' bar, over them, shown while it is wanted

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

    // The view scrolled or zoomed sideways by the user, as a scroll bar does it: following the playhead pauses (the playlist's
    // overview of the song does it too).
    void pauseFollowing();

    // What the editor shows, the whole of it, in miniature along its time bar's top, as FL Studio's playlist and piano roll show it
    // under their scroll bar there, and code editors' minimaps a file (the owner, 2026-11-06): each mark a clip or a note, from
    // beat to beat, across a share of its height, in its colour; the part in view a see-through thumb over them, which drags to
    // scroll the editor, either of its ends to zoom it; a press elsewhere brings that part into view, centred, and drags on from
    // there; the wheel scrolls. It takes the place of a scroll bar under the editor: the derived class shows it (getOverview()),
    // hides horizontalScrollBar, and calls overviewContentChanged() when what it gives changes.
    struct OverviewMark
    {
        double start = 0.0, end = 0.0;   // in beats, from the song's start
        float top = 0.0f, bottom = 1.0f; // its share of the overview's height, from the top
        juce::Colour colour;
        bool faint = false; // muted
    };

    virtual std::vector<OverviewMark> getOverviewMarks() const { return {}; }
    void overviewContentChanged();
    juce::Component& getOverview() noexcept;
    static constexpr int overviewHeight = 20;

    // A scroll bar over what it scrolls, thin and see-through, as FL Studio's are: its colours, again for another theme.
    static void makeSeeThrough (juce::ScrollBar&);

private:
    struct Playhead;
    struct UseListener;
    struct Overview;

    CommandBridge& commands;
    const GridOwner gridOwner;
    ProjectContext& timelineContext;
    TimelineView view;
    BeatPosition timeOrigin;
    GridSettings grid;
    Tool tool = Tool::smart;
    std::optional<Tool> toolBeforeKey; // while a tool's key is down: the tool to spring back to
    bool usedWhileKeyDown = false;     // and whether the mouse was pressed meanwhile
    std::optional<Tool> toolAfterDrag; // the key was let go during a drag: the tool to go back to when it ends
    bool mouseIsDown = false;
    bool holdsTimeline = false; // the mouse went down on a part that draws the time line, and is down still: the view stays put
    bool followPlayback = true, active = false, scrollContinuously = false;
    bool followPaused = false, playedWhenFollowed = false; // by a scroll while playing; and whether it played last time
    bool layingOutForScrollBar = false, scrollBarChangedMeanwhile = false;
    double contentEnd = 0.0;            // in beats, as the scroll bar knows it
    std::optional<BeatRange> shownLoop; // the loop the ruler shows (none while playback doesn't loop), to repaint it on changes
    bool shownPunch = false;            // whether it shows the loop red, Record punching into it (RecordingModel.h)
    juce::Time lastAutoScrollTime;      // the drag's before (juce::MouseEvent::eventTime)
    double autoScrollRest = 0.0;        // the part of a pixel a drag past an edge has scrolled, which the view keeps till it is one
    double autoScrollRestY = 0.0;       // the same up and down, the rows' scrolling being by whole pixels too
    double wheelRest = 0.0;             // the same for the wheel (a trackpad's moves are parts of a notch)
    std::unique_ptr<Playhead> playhead;
    std::unique_ptr<juce::Component> overview; // an Overview (TimelineOverview.cpp), made as the derived class first asks for it
    std::vector<std::unique_ptr<TimelinePartWindow>> partWindows; // addTimelinePart()
    juce::VBlankAttachment playheadFrames;                        // showFrame() at each frame of the display, while a project shows
    std::unique_ptr<UseListener> useListener;
    std::optional<BeatPosition> frameBeat; // while playing, the beat heard at the frame shown last (showFrame())
    bool holdsPlayhead = false;            // scrolling continuously, the view moves under the playhead, which stays put
    double playheadX = 0.0;                // getPlayheadX()
    float subPixel = 0.0f;                 // getSubPixel()
    FrameLog frameLog;
    void setSubPixel (float);                                          // moves the time line's parts left by it, in their windows
    TimelinePartWindow* findPartWindow (const juce::Component&) const; // the window a part added shows through
    bool isInTimelinePart (const juce::Component*) const;              // a part added, or inside one
    int viewStep = 1;                                                  // getViewStep()
    void updateViewStep();                                             // for the display's scale now

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void componentVisibilityChanged (juce::Component&) override; // the vertical scroll bar showed or hid itself
    void timerCallback() override;                               // notices what changed elsewhere, a few times a second
    void followPlayhead();                                       // moves the playhead, and turns the page while playing
    void writeFrameLog();                                        // the frames since playback started, into Phazite's log

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
