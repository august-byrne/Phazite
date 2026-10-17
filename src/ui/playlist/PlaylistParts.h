// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's parts, for its own .cpp files: Playlist.cpp is the view (the lanes, the panel's layout, and what it
// listens to), PlaylistRows.cpp lays out the tracks' rows and their clips, PlaylistTools.cpp is the tools (the mouse in
// the lanes), PlaylistDrops.cpp takes files and browser items dropped on it, PlaylistAutomation.cpp shows the automation
// lanes under the tracks and edits them, and PlaylistTracks.cpp handles the track headers and their menus, and holds the
// playlist's commands.
#pragma once

#include "Playlist.h"
#include "ui/timeline/TimelineGesture.h"

#include <map>

namespace phazite::ui
{
//==============================================================================
// The tracks' rows, with the grid, and the clips on them. The hint bar shows what the mouse does where
// it is (the description changes as it moves).
struct Playlist::Lanes : public juce::Component
{
    explicit Lanes (Playlist&);

    void paint (juce::Graphics&) override;

    // Where a row is in the lanes, cut to the part that shows: scrolling rows don't show in the pinned band.
    juce::Rectangle<int> getRowArea (const Row&) const;

    // Over the clips: the time range selected across tracks, and the Select or Zoom tool's rectangle.
    void paintOverChildren (juce::Graphics&) override;

    // The tools' work is the playlist's.
    void mouseMove (const juce::MouseEvent& e) override { playlist.lanesMouseMove (e); }
    void mouseDown (const juce::MouseEvent& e) override { playlist.lanesMouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { playlist.lanesMouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { playlist.lanesMouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { playlist.lanesDoubleClick (e); }

    Playlist& playlist;
};

//==============================================================================
// After the last track's header: a row that adds a track at the end, as the space after the tracks does in
// Logic, Cubase and REAPER. Right-clicking it offers a folder or a return track too.
struct Playlist::AddTrackRow : public juce::Button
{
    explicit AddTrackRow (Playlist&);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void mouseDown (const juce::MouseEvent&) override;

    Playlist& playlist;
};

//==============================================================================
// A lane's automation (docs/design.md, section 10), under its track's clips. It shows the points and the line between
// them as the engine plays it, in the track's color. The tools edit it. The Smart Tool needs no keys: a click adds a
// point (pressed on the line, the new point sits on it, and a drag pulls it), a point drags, the handle in the middle of
// the line under the mouse bends that line, a drag on empty space selects the points in a rectangle, and a double-click
// deletes a point or straightens a line at its handle. Draw draws steps on the grid (or, with Alt, a free line); Paint
// draws the chosen shape; Delete deletes the points it passes; Select selects. Each drag is one undo step. Collapsed, it
// shows the line in a thin row, and a double-click expands it.
struct Playlist::AutomationLaneView : public juce::Component,
                                      public juce::SettableTooltipClient
{
    AutomationLaneView (Playlist&, TrackID, const ParameterRef&);

    void update (juce::Colour, bool isCollapsed); // its color, whether it is a thin row, and what it shows now

    // A click's point where nothing is: on the grid unless Alt is held, as its own undo step, and selected.
    void addPointAt (juce::Point<float>, const juce::ModifierKeys&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // What a press does once it drags.
    enum class Drag
    {
        none,
        pending, // not dragging yet: a click, unless the mouse moves far enough
        movePoints,
        bend,
        draw,
        paint,
        select,
        erase,
        forward // to the playlist's own tools (Zoom)
    };

    Playlist& playlist;
    const TrackID track;
    const ParameterRef parameter;
    ParameterHandle handle; // its range, value and texts
    ParameterKind kind = ParameterKind::other;
    juce::Colour colour;
    bool collapsed = false;
    std::vector<AutomationPoint> points; // as shown

    Drag drag = Drag::none, pendingDrag = Drag::none;
    TimelineGesture gesture;
    juce::Point<float> start;            // where the mouse went down
    std::vector<AutomationPoint> before; // the points then
    std::vector<int> moving;             // the points that move
    int pressed = -1;                    // the point, or the line after the point, the mouse went down on
    int hovered = -1;                    // the line, by the point it starts at, whose bend handle shows: the one under the mouse
    bool clickSelectsAlone = false;      // a click on a selected point selects it alone, unless it starts a drag
    std::map<double, float> steps;       // Draw: each grid cell's value, by its start in beats
    std::vector<AutomationPoint> stroke; // Draw with Alt: the free line, by beat
    std::optional<BeatRange> drawn;      // Draw and Paint: what they have replaced so far
    std::vector<int> selectedBefore;     // Select with Ctrl: the points selected when it began
    juce::Rectangle<float> rubberBand;   // Select

    // Geometry: the curve's area, and where values and beats are in it.
    juce::Rectangle<float> getCurveArea() const;
    float valueToY (float) const;
    float yToValue (float) const;
    float beatToX (BeatPosition) const;
    BeatPosition xToBeat (float) const;
    BeatPosition limitBeat (BeatPosition) const; // where points can go: the song
    std::optional<int> getPointAt (juce::Point<float>) const;
    std::optional<float> getValueAt (BeatPosition) const;         // the line's, as the engine plays it
    bool isOnLine (juce::Point<float>) const;                     // anywhere along it, before the first point and after the last too
    int getLineUnder (float x) const;                             // the point that starts the line across x, or -1
    std::optional<juce::Point<float>> getHandle (int line) const; // in its middle, unless too short or flat to bend
    std::optional<int> getHandleAt (juce::Point<float>) const;    // the line whose handle is under the pointer
    juce::String getValueText (float) const;
    juce::String describeHere (juce::Point<float>) const; // for the hint bar
    void setHovered (int line);

    void pressPoint (int index, const juce::ModifierKeys&); // selects it as a click does, ready to move it
    void pullPointFrom (const juce::MouseEvent&);           // a press on the line: a point there, which a drag moves
    void beginDrag (const juce::MouseEvent&);
    void dragPoints (const juce::MouseEvent&);
    void bendLine (const juce::MouseEvent&);
    void drawTo (const juce::MouseEvent&);
    void paintTo (const juce::MouseEvent&);
    void selectIn (juce::Rectangle<float>, bool addToSelection);
    void eraseAt (juce::Point<float>);
    void replaceDrawn (BeatRange, std::vector<AutomationPoint>); // keeps what was there outside it, as it was
    void refreshPoints();
};

//==============================================================================
// An automation lane's header, under its track's header: a chevron that collapses the lane to a thin row, as a track's
// does, the parameter it shows (in a lane without automation, click for another of the track's), and the track's write
// mode. Dragging its bottom edge resizes the lane, collapsing it as a track collapses; right-clicking shows its menu.
struct Playlist::AutomationLaneHeader : public juce::Component
{
    AutomationLaneHeader (Playlist&, TrackID, const ParameterRef&);

    // The track's color, depth and write mode, the parameter's name, and whether the lane is collapsed.
    void update (const TrackRow&, const LaneRow&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    Playlist& playlist;
    const TrackID track;
    const ParameterRef parameter;
    TrackRow row;
    bool collapsed = false, automated = false;
    IconButton collapseButton { "Collapse lane", Icon::expanded };
    juce::TextButton nameButton, modeButton;
    bool resizing = false;
    int heightAtDragStart = 0;

    void showParameterMenu();
    void showModeMenu();
    bool isOnResizeEdge (juce::Point<int>) const;
};

//==============================================================================
// What the mouse is doing in the lanes, from the moment it goes down until it goes up.
struct Playlist::Gesture : public TimelineGesture
{
    enum class Kind
    {
        pending, // not dragging yet: a click, unless the mouse moves far enough
        move,
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        selectRange,
        crossfade,
        slip,   // the Slip tool
        paint,  // the Paint tool
        mute,   // the Mute tool, over clips
        erase,  // the Delete tool (or right-click, in FL Studio's keymap), over clips
        select, // the Select tool's rectangle
        zoom    // the Zoom tool's range
    };

    // A clip being moved, and where it started: its row, and its track and place there, whose automation goes with it.
    struct MovingClip
    {
        ClipID clip;
        BeatPosition start;
        int row = -1;
        ClipPlace place {};
    };

    // Which ways the lanes scroll when this drags past their edges, if at all.
    std::optional<TimelineEditor::ScrollDirections> getAutoScrollDirections() const
    {
        switch (kind)
        {
            case Kind::move:
            case Kind::selectRange:
            case Kind::mute:
            case Kind::erase:
            case Kind::select:
                return TimelineEditor::ScrollDirections::both;
            case Kind::trimStart:
            case Kind::trimEnd:
            case Kind::paint:
            case Kind::zoom:
                return TimelineEditor::ScrollDirections::sideways;
            case Kind::pending:
            case Kind::fadeIn:
            case Kind::fadeOut:
            case Kind::crossfade:
            case Kind::slip:
            default:
                return std::nullopt;
        }
    }

    Kind kind = Kind::pending;
    ClipID clip;                  // the clip the mouse went down on, if any
    std::optional<ClipZone> zone; // and where on it
    int startRow = -1;            // the row it went down on

    std::vector<MovingClip> moving;
    BeatPosition anchorStart; // where the clip under the mouse (or its copy) started; it is the one that snaps
    int rowOffset = 0;        // how many rows the moving clips have moved
    BeatDuration moved;       // and how far along
    bool copying = false;     // copies (or aliases) of the clips move, and the originals stay

    std::optional<CrossfadeSpot> crossfade; // where the mouse went down to draw a crossfade
    double startOffset = 0.0;               // Slip: the clip's offset when the drag began, in seconds

    // Select with Ctrl: what was selected already.
    std::vector<TrackID> tracksBefore;
    std::vector<ClipID> clipsBefore;
    std::vector<SectionID> sectionsBefore;
};
} // namespace phazite::ui
