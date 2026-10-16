// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's parts, for its own .cpp files: Playlist.cpp is the view (the lanes, the panel's layout, and what it
// listens to), PlaylistRows.cpp lays out the tracks' rows and their clips, PlaylistTools.cpp is the tools (the mouse in
// the lanes), PlaylistDrops.cpp takes files and browser items dropped on it, and PlaylistTracks.cpp handles the track
// headers and their menus, and holds the playlist's commands.
#pragma once

#include "Playlist.h"
#include "ui/timeline/TimelineGesture.h"

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

    // A clip being moved, and where it started.
    struct MovingClip
    {
        ClipID clip;
        BeatPosition start;
        int row = -1;
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

    std::optional<CrossfadeSpot> crossfade; // where the mouse went down to draw a crossfade
    double startOffset = 0.0;               // Slip: the clip's offset when the drag began, in seconds

    // Select with Ctrl: what was selected already.
    std::vector<TrackID> tracksBefore;
    std::vector<ClipID> clipsBefore;
    std::vector<SectionID> sectionsBefore;
};
} // namespace phazite::ui
