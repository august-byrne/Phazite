// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist: the song's tracks and clips on a timeline, with a ruler, the grid, the playhead and a
// header for each track (roadmap step 1.1). Its tools edit the clips (model/SmartTool.h): by default the
// Smart Tool, where the spot a drag starts on a clip decides what it does. Folders come in the step's last part.
#pragma once

#include "ClipComponent.h"
#include "Icons.h"
#include "TrackHeader.h"
#include "model/Commands.h"
#include "model/Selection.h"
#include "model/SmartTool.h"
#include "model/Timeline.h"

#include <optional>

namespace phazite::ui
{
class Playlist : public juce::Component,
                 private TrackHeader::Owner,
                 private juce::ChangeListener,
                 private juce::ValueTree::Listener,
                 private juce::AsyncUpdater,
                 private juce::ScrollBar::Listener,
                 private juce::Timer
{
public:
    Playlist (CommandRegistry&, te::SelectionManager&, RangeSelection&);
    ~Playlist() override;

    // The open project's Edit, or nullptr while none is open.
    void setEdit (te::Edit*);

    // Adds the playlist's commands: renaming and coloring tracks, zooming, following playback, the grid,
    // the tools, and nudging and quantizing clips.
    void addCommands (CommandRegistry&);

    // Puts the current shortcuts into the tooltips; call after the keymap changed.
    void refreshShortcutTexts();

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override; // where a dragged track would go
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Ruler;
    struct Lanes;
    struct Playhead;
    struct Gesture;

    struct Row
    {
        te::SafeSelectable<te::Track> track;
        int top = 0, height = 0; // in the track area, before scrolling
        juce::Colour colour;
    };

    // Where two audio clips on a track meet (they touch, or overlap in a crossfade): dragging near the
    // bottom of the boundary between them draws a crossfade.
    struct CrossfadeSpot
    {
        te::SafeSelectable<te::Clip> first, second;
        te::TimePosition boundary;
    };

    CommandRegistry& commands;
    te::SelectionManager& selection;
    RangeSelection& range; // a time range across tracks
    te::SafeSelectable<te::Edit> edit;
    juce::ValueTree editState; // listened to for changes to the tracks and the tempo

    TimelineView view;
    GridSettings grid;
    int scrollY = 0;
    bool followPlayback = true, wasPlaying = false;
    double songEnd = 0.0; // in beats, as the scroll bar knows it

    IconButton addTrackButton { "Add track", Icon::plus }, snapButton { "Snap to grid", Icon::magnet };
    juce::TextButton gridButton;
    juce::OwnedArray<IconButton> toolButtons; // one for each of allTools, in order
    Tool tool = Tool::smart;
    te::SafeSelectable<te::Clip> lastClip; // the clip the Draw and Paint tools place copies of
    std::unique_ptr<Ruler> ruler;
    juce::Component headerColumn, headerContent; // headerContent holds the headers and scrolls inside headerColumn
    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<Playhead> playhead;
    juce::ScrollBar verticalScrollBar { true }, horizontalScrollBar { false };

    std::vector<Row> rows;
    juce::OwnedArray<TrackHeader> headers; // headers[i] shows rows[i]
    juce::OwnedArray<ClipComponent> clipComponents;
    int contentHeight = 0;
    te::SafeSelectable<te::Track> selectionAnchor; // where Shift-click ranges start
    int dropRow = -1;                              // while a track is dragged: the row it would go before (rows.size() for the end)
    std::unique_ptr<Gesture> gesture;              // what the mouse is doing in the lanes, from mouse down to mouse up
    juce::Rectangle<int> rubberBand;               // the Select and Zoom tools' rectangle, in the lanes

    // The rows of tracks.
    void rebuild(); // after tracks were added, deleted, moved or changed
    void layOutRows();
    void updateHeaders();
    int getRowAt (int y) const; // y in the lanes; -1 below the last row
    int getRowOf (const te::Track*) const;
    TrackHeader* getHeaderFor (const te::Track*) const;
    void setScrollY (int);
    void scrollToShow (int row);
    void updateScrollBars();

    // The clips.
    void rebuildClips();
    void layOutClips();
    void updateClips();                                   // their colors and whether they are selected
    juce::Rectangle<int> getClipBounds (te::Clip&) const; // in the lanes
    ClipComponent* getClipComponentAt (juce::Point<int>) const;
    ClipComponent* getClipComponentFor (const juce::ValueTree& clipState) const;
    void clipChanged (const juce::ValueTree& clipState);
    void clipClicked (te::Clip&, const juce::ModifierKeys&);
    void showClipMenu();

    // The tools: the mouse in the lanes.
    void setTool (Tool);
    void lanesMouseMove (const juce::MouseEvent&);
    void lanesMouseDown (const juce::MouseEvent&);
    void lanesMouseDrag (const juce::MouseEvent&);
    void lanesMouseUp (const juce::MouseEvent&);
    void lanesDoubleClick (const juce::MouseEvent&);
    void smartToolDown (const juce::MouseEvent&);
    void beginDrag (const juce::MouseEvent&);
    std::optional<CrossfadeSpot> findCrossfadeAt (juce::Point<int>) const;
    void selectRange (te::BeatPosition from, te::BeatPosition to, int fromRow, int toRow);
    te::Clip* placeClip (int row, te::BeatPosition, bool snapStart); // a copy of lastClip, for Draw and Paint
    void muteOrDeleteClipAt (juce::Point<int>, bool deleteIt);       // once per gesture for each clip
    void selectClipsIn (juce::Rectangle<int>, bool addToSelection);
    void nudge (int direction); // the selected clips, or the range, by a step of the grid
    void quantizeSelectedClips();

    // Time and the grid.
    te::TimecodeSnapType getSnapType() const;
    te::BeatPosition snap (te::BeatPosition, juce::ModifierKeys) const; // Alt places freely
    double getSongEndInBeats() const;
    void setViewStart (te::BeatPosition);
    void zoomAround (double x, double factor);
    void zoom (double factor); // around the playhead if it is in view, otherwise the middle
    void zoomToFit();
    void centreOnPlayhead();
    void movePlayheadTo (te::BeatPosition);
    void updatePlayhead();
    void setGrid (const GridSettings&); // and keeps it in the project
    void updateGridControls();
    void showGridMenu();
    void setColourOfSelectedTracks (juce::Colour);

    // TrackHeader::Owner
    void trackClicked (te::Track&, const juce::ModifierKeys&) override;
    void showTrackMenu (te::Track&) override;
    void showColourMenu (te::Track&, juce::Component& target) override;
    void trackDragged (TrackHeader&, const juce::MouseEvent&) override;
    void trackDragEnded (TrackHeader&, const juce::MouseEvent&) override;
    void trackHeightChanged() override { layOutRows(); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection or the range changed
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void handleAsyncUpdate() override { rebuild(); }
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override; // moves the playhead, and follows it while playing

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Playlist)
};
} // namespace phazite::ui
