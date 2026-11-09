// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist: the song's tracks and clips on a time line, with a header for each track (roadmap step 1.1).
// Its tools edit the clips (ui/timeline/SmartTool.h): by default the Smart Tool, where the spot a drag starts on
// a clip decides what it does. The time line, the grid and the tools come from TimelineEditor. It works on the
// model's playlist by IDs (model/arrangement/PlaylistModel.h).
#pragma once

#include "ChordLane.h"
#include "ClipComponent.h"
#include "SectionsStrip.h"
#include "TempoLane.h"
#include "TrackHeader.h"
#include "model/automation/AutomationModel.h"
#include "model/project/ProjectContext.h"
#include "ui/components/DisplayTicker.h"
#include "ui/timeline/TimelineEditor.h"

#include <optional>
#include <unordered_map>

namespace phazite::ui
{
class Playlist : public TimelineEditor,
                 public juce::FileDragAndDropTarget,
                 public juce::DragAndDropTarget,
                 public juce::DragAndDropContainer,
                 private TrackHeader::Owner,
                 private juce::ChangeListener,
                 private juce::AsyncUpdater,
                 private ProjectContext::Listener,
                 private DisplayTicker::Display
{
public:
    // Shows the open project's tracks and clips, and none while no project is open.
    Playlist (CommandBridge&, ProjectContext&);
    ~Playlist() override;

    // Called when a clip is double-clicked, to open it in the editor dock: the piano roll or the audio clip editor.
    std::function<void (ClipID)> onOpenClip;

    // Adds an audio or MIDI file at the play start (where Space plays from), as the browser's Enter does: onto the
    // selected track if it suits the file, or a new track after it. Returns false if nothing could be added.
    bool placeFile (const juce::File&);

    // How wide the track headers' column is: its right edge drags it, from minHeaderWidth to maxHeaderWidth, and a
    // double-click on it puts it back, as in Logic Pro and REAPER. The app keeps it in its settings (onHeaderWidthChanged).
    int getHeaderWidth() const noexcept { return headerWidth; }
    void setHeaderWidth (int);
    std::function<void (int)> onHeaderWidthChanged; // as the drag ends
    static constexpr int defaultHeaderWidth = 200, minHeaderWidth = 150, maxHeaderWidth = 360;

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
    void paintOverChildren (juce::Graphics&) override; // where dragged tracks would go
    void resized() override;
    void lookAndFeelChanged() override { updateScrollBarColours(); } // another theme: its scroll bar's colours in it
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    int headerWidth = defaultHeaderWidth;
    static constexpr double scrollPerNotch = 48.0;

    // Its parts (PlaylistParts.h).
    struct HeaderEdge; // the headers' column's right edge, which drags its width
    struct Lanes;
    struct Gesture;
    struct AddTrackRow;
    struct AutomationLaneView;   // a lane's curve, under its track's clips
    struct AutomationLaneHeader; // the lane's header, under its track's header

    // An automation lane, under its track's row (model/automation/AutomationModel.h).
    struct LaneRow
    {
        AutomationLane lane;
        int top = 0, height = 0; // from the top of its track's row; no height while its track is out of focus
        bool collapsed = false;  // a thin row: collapsed itself, or its track is
    };

    // A track shown in the playlist, with its lanes under it. Pinned tracks stay in a band at the top; the rest scroll
    // under it.
    struct Row
    {
        TrackRow track;
        int top = 0, height = 0; // in the pinned band, or among the scrolling rows before scrolling; the height is the
                                 // track's own, where its clips are
        std::vector<LaneRow> lanes {};
        int lanesHeight = 0;

        int getTotalHeight() const noexcept { return height + lanesHeight; }
    };

    ProjectContext& context;
    SongSelection& selection; // the context's, by IDs
    SongRange& range;         // a time range across tracks
    int shownProject = -1;    // the project shown (ProjectContext::getProjectNumber()), to tell another one from the same one
    int scrollY = 0;

    SectionsStrip sectionsStrip; // in the time bar, above the ruler
    TempoLane tempoLane;         // under the time bar, above the chord lane and the tracks, while the song's tempo changes
    TempoLaneHeader tempoLaneHeader;
    bool tempoLaneShown = false;
    void showTempoLaneWithChanges(); // shows the tempo lane, or takes it away, as the song has more than one tempo or not

    ChordLane chordLane; // under the time bar, above the tracks, while the song has chords
    ChordLaneHeader chordLaneHeader;
    bool chordLaneShown = false;
    void showChordLaneWithChords(); // shows the chord lane, or takes it away, as the song has chords or not

    IconButton addTrackButton { "Add track", Icon::plus };              // over the track headers, left of the time bar
    IconButton collapseAllButton { "Collapse all", Icon::collapseAll }; // beside it: every row thin, or every one expanded
    ClipID lastClip;                                                    // the clip the Draw and Paint tools place copies of
    bool loopsSelection = false; // with a keymap whose selection loops: the loop is the selected time range's

    // The header column holds the pinned headers in a band at the top, and below it the scrolling
    // headers, inside headerContent, with a row after them that adds a track; the lanes hold the clips in two
    // layers the same way.
    juce::Component headerColumn, pinnedHeaders, scrollingHeaders, headerContent;
    std::unique_ptr<AddTrackRow> addTrackRow;
    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<HeaderEdge> headerEdge;
    void keepHeaderWidth();        // tells the app the width the headers' edge was dragged to
    void updateScrollBarColours(); // the rows' scroll bar's, see-through over the lanes
    juce::Component pinnedClips, scrollingClips;

    std::vector<Row> rows;
    std::unordered_map<TrackID, int> rowOf;              // rows[i] by its track
    juce::OwnedArray<TrackHeader> headers;               // headers[i] shows rows[i]
    juce::OwnedArray<AutomationLaneHeader> laneHeaders;  // the rows' lanes, in their order
    juce::OwnedArray<AutomationLaneView> laneViews;      // laneViews[i] goes with laneHeaders[i]
    AutomationShape paintShape = AutomationShape::sine;  // what the Paint tool draws in a lane
    std::optional<std::vector<ParameterRef>> knownLanes; // the lanes shown when they were built last: none for a new project

    // The clips on the rows, row by row as the model lists them, which the tools find and select, and where each is in that
    // list. Clips that overlap show one under another in their row (getClipSubRows()); where two crossfade, a later one
    // shows over an earlier one. Only the clips in view have components, made as they come into view: a song can hold
    // thousands of clips (docs/performance-audit.md, section 8). An audio clip's waveform stays until the clip leaves the
    // rows, so that it doesn't load again each time the clip scrolls back into view.
    std::vector<ClipInfo> shownClips;
    std::vector<ClipSubRow> shownSubRows; // shownSubRows[i] is shownClips[i]'s
    std::unordered_map<ClipID, size_t> clipIndex;
    std::unordered_map<ClipID, std::unique_ptr<ClipComponent>> clipComponents;
    std::unordered_map<ClipID, juce::Rectangle<int>> shownParts; // what showed of each, in its own coordinates (layOutClips())
    std::unordered_map<ClipID, std::unique_ptr<ClipWaveform>> waveforms;
    std::unordered_map<TrackID, FolderSummary> folderSummaries; // what each folder's row shows of its tracks' clips

    // Where audio clips on a track meet, by track, as the model found them at the zoom of crossfadeZoom (its pixels a beat),
    // asked for when the mouse first comes over the track: a mouse move asks the model nothing, which made a move over a
    // track of a dozen audio clips take a frame's time (docs/performance-audit.md, section 16). rebuildClips() forgets them.
    std::unordered_map<TrackID, std::vector<CrossfadeSpot>> crossfadeSpots;
    double crossfadeZoom = 0.0;

    int contentHeight = 0; // of the scrolling rows
    int pinnedHeight = 0;  // of the pinned band
    bool focusMode = false;
    std::optional<BeatPosition> fileDropBeat; // where files dragged over the playlist would go
    int fileDropRow = -1;
    bool deviceDropping = false; // a device from the browser is dragged over the playlist, to fileDropRow
    TrackID selectionAnchor;     // where Shift-click ranges start

    // While tracks are dragged by a header: where they would go, at a row's track (TrackDrop, model/arrangement/PlaylistModel.h);
    // nothing where they can't go. Before or after a track by the half of it the pointer is in; on a folder, into it in its
    // middle, and before or after it in its top and bottom quarters (before its first track while that shows); under the
    // tracks, at the end.
    struct TrackDropPlace
    {
        int row = -1;
        TrackDrop drop = TrackDrop::before;
    };

    std::optional<TrackDropPlace> trackDrop;
    std::optional<TrackDropPlace> findTrackDrop (int y) const; // y in the lanes
    void fadeMovingHeaders (bool);                             // the selected tracks' while they move, and those in them
    bool headersFaded = false;
    void paintTrackDrop (juce::Graphics&) const;

    std::unique_ptr<Gesture> gesture; // what the mouse is doing in the lanes, from mouse down to mouse up
    juce::Rectangle<int> rubberBand;  // the Select and Zoom tools' rectangle, in the lanes

    void showProject(); // the open project's, or none
    void projectChanged() override { showProject(); }
    void songChanged (const SongChanges&) override;

    // Audio clips whose renders are being made (model/arrangement/AudioClipModel.h): silent until then, and shown so, with
    // how far each has come: asked at every third of the ticker's ticks (ten times a second), which walks the song's audio
    // clips while anything renders, and nothing otherwise.
    bool showsRendering = false;
    int ticksToSkip = 0;
    void tick() override;

    // What is being recorded, on each track it records onto, growing as it comes in, as Ableton Live draws it
    // (PlaylistRecording.cpp): a clip that shows it (ClipComponent::showRecording()), from where it started to the
    // playhead, brought up to date at each of the ticker's ticks, and its end at each frame of the display.
    struct ShownRecording;
    std::vector<ShownRecording> shownRecordings;
    void showRecordings();
    void layOutRecordings();
    void playheadShown() override;

    // The rows of tracks.
    std::vector<TrackRow> getShownRows() const; // the playlist's tracks, and the master while it has lanes
    void rebuild();                             // after tracks were added, deleted, moved or changed
    void layOutRows();
    void updateHeaders();
    void updateCollapseAllButton(); // Collapse All while any row is expanded, Expand All otherwise

    // The automation lanes (PlaylistAutomation.cpp).
    void refreshLanes(); // the rows' lanes as the model has them now, laid out; it scrolls to show a lane shown just now
    void rebuildLanes(); // their headers and views, keeping those of lanes still shown
    void layOutLanes();
    void updateLanes();                               // their colors, modes, selected points and curves
    void noteLanes();                                 // after the lanes were built: one shown since the last time scrolls into view
    void showLaneMenu (TrackID, const ParameterRef&); // a lane's right-click menu
    void setLaneCollapsed (TrackID, const ParameterRef&, bool collapse); // expanding one opens a collapsed track
    int getRowAt (int y) const;                                          // y in the lanes; -1 below the last row
    int getRowOf (TrackID) const;
    int getRowTop (const Row&) const; // where a row is in the lanes now
    bool isInFocus (TrackID) const;   // in focus mode, the rest are made small
    TrackHeader* getHeaderFor (TrackID) const;
    void setScrollY (int);
    void scrollToShow (int row);
    void updateVerticalScrollBar();

    // The clips.
    void rebuildClips(); // the clips on the rows, as they are now
    void layOutClips();  // a component for each clip in view, where it is
    std::unique_ptr<ClipComponent> makeClipComponent (const ClipInfo&);
    void waveformLoaded (ClipID);                                        // more of an audio clip's waveform loaded: its component paints it
    int getZOrderFor (const juce::Component& layer, size_t index) const; // after the clips before it in the list
    void updateClips();                                                  // their colors and whether they are selected
    void showLookOf (ClipComponent&) const;
    juce::Rectangle<int> getClipBounds (const ClipInfo&) const; // in the lanes, in its sub-row
    const ClipInfo* getClipAt (juce::Point<int>) const;         // the one shown on top there, or nullptr
    void clipClicked (ClipID, const juce::ModifierKeys&);
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
    std::optional<CrossfadeSpot> findCrossfadeAt (juce::Point<int>);
    void selectRange (BeatPosition from, BeatPosition to, int fromRow, int toRow);
    ClipID placeClip (int row, BeatPosition, bool snapStart);  // a copy of lastClip, for Draw and Paint
    void muteOrDeleteClipAt (juce::Point<int>, bool deleteIt); // once per gesture for each clip
    void selectClipsIn (juce::Rectangle<int>, bool addToSelection);

    // Audio and MIDI files dropped from Explorer: onto a track that suits them, or onto new tracks.
    void importFiles (const juce::StringArray& paths, juce::Point<int> position);
    bool addFiles (const juce::StringArray& paths, TrackID target, BeatPosition start); // as one undo step
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
    BeatPosition getFileDropBeat (int x) const; // at the grid line at or before x, unless Alt is held

    void setColourOfSelectedTracks (juce::Colour);
    void showRulerMenu (BeatPosition) override; // the sections there, the song outlines and the loop

    // After the last track: a track or a folder (and it shows), and the menu of what can be added there.
    void addTrackAtEnd (bool folder);
    void showAddTrackMenu();

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
    std::vector<juce::Component*> getScrollingParts() override;  // the lanes, the sections, the tempo lane and the chord lane
    std::vector<OverviewMark> getOverviewMarks() const override; // each track's clips, a row each
    BeatRange getContentRange() const override;
    void timelineChanged() override;
    void toolChanged() override;
    void gridChanged() override;
    void verticalScrollBarMoved (double newRangeStart) override;
    bool scrollVerticallyBy (int pixels) override;

    // TrackHeader::Owner
    void trackClicked (TrackID, const juce::ModifierKeys&) override;
    void showTrackMenu (TrackID) override;
    void showColourMenu (TrackID, juce::Component& target) override;
    void trackDragged (TrackHeader&, const juce::MouseEvent&) override;
    bool trackDraggedOut (TrackHeader&, const juce::MouseEvent&) override;
    void trackDragEnded (TrackHeader&, const juce::MouseEvent&) override;
    void trackHeightChanged() override; // the row's height, as the model keeps it now

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection or the range changed
    void handleAsyncUpdate() override { rebuild(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Playlist)
};
} // namespace phazite::ui
