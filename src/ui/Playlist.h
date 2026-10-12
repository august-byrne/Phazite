// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist: the song's tracks on a timeline, with a ruler, the grid, the playhead and a header for
// each track (roadmap step 1.1). Clips, the tools and folders come in the step's later parts.
#pragma once

#include "Icons.h"
#include "TrackHeader.h"
#include "model/Commands.h"
#include "model/Timeline.h"

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
    Playlist (CommandRegistry&, te::SelectionManager&);
    ~Playlist() override;

    // The open project's Edit, or nullptr while none is open.
    void setEdit (te::Edit*);

    // Adds the playlist's commands: renaming and coloring tracks, zooming, following playback and the grid.
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

    struct Row
    {
        te::SafeSelectable<te::Track> track;
        int top = 0, height = 0; // in the track area, before scrolling
        juce::Colour colour;
    };

    CommandRegistry& commands;
    te::SelectionManager& selection;
    te::SafeSelectable<te::Edit> edit;
    juce::ValueTree editState; // listened to for changes to the tracks and the tempo

    TimelineView view;
    GridSettings grid;
    int scrollY = 0;
    bool followPlayback = true, wasPlaying = false;
    double songEnd = 0.0; // in beats, as the scroll bar knows it

    IconButton addTrackButton { "Add track", Icon::plus }, snapButton { "Snap to grid", Icon::magnet };
    juce::TextButton gridButton;
    std::unique_ptr<Ruler> ruler;
    juce::Component headerColumn, headerContent; // headerContent holds the headers and scrolls inside headerColumn
    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<Playhead> playhead;
    juce::ScrollBar verticalScrollBar { true }, horizontalScrollBar { false };

    std::vector<Row> rows;
    juce::OwnedArray<TrackHeader> headers; // headers[i] shows rows[i]
    int contentHeight = 0;
    te::SafeSelectable<te::Track> selectionAnchor; // where Shift-click ranges start
    int dropRow = -1;                              // while a track is dragged: the row it would go before (rows.size() for the end)

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

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
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
