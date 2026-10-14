// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist: the song's tracks and clips on a time line, with a header for each track (roadmap step 1.1).
// Its tools edit the clips (ui/timeline/SmartTool.h): by default the Smart Tool, where the spot a drag starts on
// a clip decides what it does. The time line, the grid and the tools come from TimelineEditor.
#pragma once

#include "ClipComponent.h"
#include "SectionsStrip.h"
#include "TrackHeader.h"
#include "model/arrangement/Selection.h"
#include "ui/timeline/TimelineEditor.h"

#include <optional>

namespace phazite::ui
{
class Playlist : public TimelineEditor,
                 public juce::FileDragAndDropTarget,
                 public juce::DragAndDropTarget,
                 private TrackHeader::Owner,
                 private juce::ChangeListener,
                 private juce::ValueTree::Listener,
                 private juce::AsyncUpdater
{
public:
    Playlist (CommandRegistry&, te::SelectionManager&, RangeSelection&);
    ~Playlist() override;

    // The open project's Edit, or nullptr while none is open.
    void setEdit (te::Edit*);

    // Called when a MIDI clip is double-clicked, to open it in the piano roll.
    std::function<void (te::MidiClip&)> onOpenClip;

    // Adds an audio or MIDI file at the play start (where Space plays from), as the browser's Enter does: onto the
    // selected track if it suits the file, or a new track after it. Returns false if nothing could be added.
    bool placeFile (const juce::File&);

    // Adds the playlist's own commands: renaming and coloring tracks, and focus mode. The time line's
    // commands are addTimelineCommands()'s.
    void addCommands (CommandRegistry&);
    void refreshShortcutTexts() override;

    // TimelineEditor
    bool canUseTool (Tool) const override { return true; }
    bool canNudgeSelection() const override;
    void nudgeSelection (int direction) override; // the selected clips, or the range
    bool canQuantizeSelection() const override;
    void quantizeSelection() override; // the selected clips' starts

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override; // where a dragged track would go
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Lanes;
    struct Gesture;
    struct AddTrackRow;

    // A track shown in the playlist. Pinned tracks stay in a band at the top; the rest scroll under it.
    struct Row
    {
        te::SafeSelectable<te::Track> track;
        int top = 0, height = 0; // in the pinned band, or among the scrolling rows before scrolling
        juce::Colour colour;
        int depth = 0; // how deep in folders
        bool pinned = false;
    };

    // Where two audio clips on a track meet (they touch, or overlap in a crossfade): dragging near the
    // bottom of the boundary between them draws a crossfade.
    struct CrossfadeSpot
    {
        te::SafeSelectable<te::Clip> first, second;
        te::TimePosition boundary;
    };

    te::SelectionManager& selection;
    RangeSelection& range;     // a time range across tracks
    juce::ValueTree editState; // listened to for changes to the tracks and the tempo
    int scrollY = 0;

    SectionsStrip sectionsStrip { *this, selection, range }; // in the time bar, above the ruler

    IconButton addTrackButton { "Add track", Icon::plus }; // over the track headers, left of the time bar
    te::SafeSelectable<te::Clip> lastClip;                 // the clip the Draw and Paint tools place copies of
    bool loopsSelection = false;                           // with a keymap whose selection loops: the loop is the selected time range's

    // The header column holds the pinned headers in a band at the top, and below it the scrolling
    // headers, inside headerContent, with a row after them that adds a track; the lanes hold the clips in two
    // layers the same way.
    juce::Component headerColumn, pinnedHeaders, scrollingHeaders, headerContent;
    std::unique_ptr<AddTrackRow> addTrackRow;
    std::unique_ptr<Lanes> lanes;
    juce::Component pinnedClips, scrollingClips;

    std::vector<Row> rows;
    juce::OwnedArray<TrackHeader> headers; // headers[i] shows rows[i]
    juce::OwnedArray<ClipComponent> clipComponents;
    int contentHeight = 0; // of the scrolling rows
    int pinnedHeight = 0;  // of the pinned band
    bool focusMode = false;
    std::optional<te::BeatPosition> fileDropBeat; // where files dragged over the playlist would go
    int fileDropRow = -1;
    bool deviceDropping = false;                   // a device from the browser is dragged over the playlist, to fileDropRow
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
    int getRowTop (const Row&) const;  // where a row is in the lanes now
    bool isInFocus (te::Track&) const; // in focus mode, the rest are made small
    TrackHeader* getHeaderFor (const te::Track*) const;
    void setScrollY (int);
    void scrollToShow (int row);
    void updateVerticalScrollBar();

    // The clips.
    void rebuildClips();
    void layOutClips();
    void positionClip (ClipComponent&);                   // in its layer, by its row's place
    void updateClips();                                   // their colors and whether they are selected
    juce::Rectangle<int> getClipBounds (te::Clip&) const; // in the lanes
    ClipComponent* getClipComponentAt (juce::Point<int>) const;
    ClipComponent* getClipComponentFor (const juce::ValueTree& clipState) const;
    void clipChanged (const juce::ValueTree& clipState);
    void clipClicked (te::Clip&, const juce::ModifierKeys&);
    void showClipMenu();

    // The tools: the mouse in the lanes.
    void lanesMouseMove (const juce::MouseEvent&);
    void lanesMouseDown (const juce::MouseEvent&);
    void lanesMouseDrag (const juce::MouseEvent&);
    void lanesMouseUp (const juce::MouseEvent&);
    void lanesDoubleClick (const juce::MouseEvent&);
    void smartToolDown (const juce::MouseEvent&);
    void beginDrag (const juce::MouseEvent&);
    juce::Point<int> getGestureStart() const; // where the mouse went down, in the lanes as they are now
    std::optional<CrossfadeSpot> findCrossfadeAt (juce::Point<int>) const;
    void selectRange (te::BeatPosition from, te::BeatPosition to, int fromRow, int toRow);
    te::Clip* placeClip (int row, te::BeatPosition, bool snapStart); // a copy of lastClip, for Draw and Paint
    void muteOrDeleteClipAt (juce::Point<int>, bool deleteIt);       // once per gesture for each clip
    void selectClipsIn (juce::Rectangle<int>, bool addToSelection);

    // Audio and MIDI files dropped from Explorer: onto a track that suits them, or onto new tracks.
    void importFiles (const juce::StringArray& paths, juce::Point<int> position);
    bool addFiles (const juce::StringArray& paths, te::Track* target, te::BeatPosition start); // as one undo step
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    // Items dragged from the browser: a sound or MIDI file goes in as a file from Explorer does; a device goes on the track
    // it is dropped on, or a new track below the last (phazite::addDeviceTo()).
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    te::BeatPosition getFileDropBeat (int x) const; // at the grid line at or before x, unless Alt is held

    void setColourOfSelectedTracks (juce::Colour);
    void showRulerMenu (te::BeatPosition) override; // the sections there, the song outlines and the loop

    // After the last track: a track or a folder (and it shows), and the menu of what can be added there.
    void addTrackAtEnd (bool folder);
    void showAddTrackMenu();

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
    te::BeatRange getContentRange() const override;
    void timelineChanged() override;
    void toolChanged() override;
    void gridChanged() override;
    void verticalScrollBarMoved (double newRangeStart) override;
    bool scrollVerticallyBy (int pixels) override;

    // TrackHeader::Owner
    void trackClicked (te::Track&, const juce::ModifierKeys&) override;
    void showTrackMenu (te::Track&) override;
    void showColourMenu (te::Track&, juce::Component& target) override;
    void trackDragged (TrackHeader&, const juce::MouseEvent&) override;
    void trackDragEnded (TrackHeader&, const juce::MouseEvent&) override;
    void trackSelectionDragged (TrackHeader&, const juce::MouseEvent&) override;
    void trackHeightChanged() override { layOutRows(); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection or the range changed
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void handleAsyncUpdate() override { rebuild(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Playlist)
};
} // namespace phazite::ui
