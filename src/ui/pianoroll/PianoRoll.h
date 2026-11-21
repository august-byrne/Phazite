// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll (docs/design.md, section 7; roadmap step 1.3): the notes of the MIDI clips selected in the
// playlist, several at once, each in its track's color, or of the selected track's clip nearest the playhead.
// Bar 1 is where the (first) clip starts, as in FL Studio and Live. FL Studio's mouse model: a click adds a
// note with the length and velocity its settings show (the note touched last's), a right-click deletes,
// dragging a note moves it (Ctrl copies, as in the playlist) and dragging its edge resizes it; Escape takes a
// drag back. It has the playlist's tools and grid (TimelineEditor), a velocity lane, velocity shown on the
// notes, ghost notes from other tracks, shading for notes outside the track's key, chord stamps, drum mode for
// drum tracks, and transforms that show what they would do before they are applied. It works by IDs, through
// the model's NotesModel.h, and selects notes through the song's selection.
#pragma once

#include "model/notes/NotesModel.h"
#include "model/project/ProjectContext.h"
#include "ui/timeline/TimelineEditor.h"

#include <map>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace phazite::ui
{
class PianoRoll : public TimelineEditor,
                  private juce::ChangeListener,
                  private ProjectContext::Listener
{
public:
    // Shows the open project's notes, and none while no project is open.
    PianoRoll (CommandBridge&, ProjectContext&);
    ~PianoRoll() override;

    // Shows these clips' notes. The piano roll follows the playlist's selection by itself; this is for
    // opening a clip, as double-clicking it in the playlist does.
    void showClips (const std::vector<ClipID>&);
    void showClip (ClipID);
    std::vector<ClipID> getClips() const; // those still in the song

    // Shows a track's MIDI clip at the playhead, or the one nearest to it. Without one, the piano roll shows
    // the track from the bar the playhead is in, and the first note drawn makes a clip there.
    void showTrack (TrackID);
    TrackID getTrack() const; // the track shown: the first clip's, or the one showTrack() was given

    // The piano roll's commands: select and deselect all notes, and the transforms with their settings.
    void addCommands (CommandRegistry&);
    void refreshShortcutTexts() override;

    // The gap Legato... was given last, which the Legato command leaves too (NotesModel.h: addNoteCommands()).
    double getLegatoGapBeats() const noexcept { return transform.legatoGapBeats; }

    // What a click adds, as its settings show it: the length and velocity of the note touched last, as in FL Studio, or
    // those chosen in its button's menu.
    BeatDuration getNextNoteLength() const noexcept { return lastLength; }
    int getNextNoteVelocity() const noexcept { return lastVelocity; }

    // The velocity lane under the notes: its height, which the line between them drags, and whether it is folded to a thin
    // row by its chevron (rows fold, never close), as the settings keep them (AppShell); and its height as it shows now.
    void setVelocityLane (int height, bool folded);
    int getVelocityLaneHeight() const noexcept;
    bool isVelocityLaneFolded() const noexcept { return velocityFolded; }
    std::function<void (int height, bool folded)> onVelocityLaneChanged; // by a drag or the chevron
    static constexpr int defaultVelocityHeight = 72, minimumVelocityHeight = 32, foldedVelocityHeight = 20;

    // What that lane shows, as the chooser beside its chevron picks it: the notes' velocities (none), or one controller's
    // values on one channel (NoteValues.h: ControlLane), as FL Studio's event lane and Ableton Live's envelope show one at a
    // time. A clip shown alone opens on the lane it was left on, as its view (ClipView), and one not seen before on the
    // velocities. And the chooser's menu (for the tests too): Velocity, the lanes the clips shown hold, the usual ones, the
    // other controllers, the channel, and Delete All of the lane's values.
    std::optional<ControlLane> getLaneShown() const noexcept { return laneShown; }
    void showLane (const std::optional<ControlLane>&);
    juce::PopupMenu getLaneMenu();

    // What a controller lane draws of a clip's values (in time order), within columns of the view, from the column of the
    // value that holds at their start: a step for each pixel column that values start in, at most one, however many start
    // there (a long take's bend zoomed out has hundreds a column). The step holds the column's last value from its column
    // to the next step's, or to the clip's end; a line up and down its column spans its lowest and highest values, with the
    // value that held before it; and it has a dot where it stands alone, its values further apart than a few pixels.
    struct ControlStep
    {
        int left = 0, right = 0;                // its columns, from its first to the next step's
        int value = 0, lowest = 0, highest = 0; // the value that holds, and the span of its column
        bool dot = false;
    };

    static std::vector<ControlStep> getControlSteps (std::span<const ControlPoint> clipValues, const TimelineView&,
                                                     juce::Range<int> columns, int clipEndColumn);

    // How far down the rows are scrolled, in pixels from the top row; set, they scroll there, within their range, as the wheel
    // and the rows' scroll bar scroll them.
    int getScrollY() const noexcept { return scrollY; }
    void setScrollY (int);

    // TimelineEditor
    bool canUseTool (Tool) const override;       // all but Slip
    bool usesSmartToolFor (Tool) const override; // Draw: the Smart Tool adds notes with a click already
    bool canNudgeSelection() const override;
    void nudgeSelection (int direction) override;
    bool canQuantizeSelection() const override;
    std::optional<BeatRange> getSelectedRange() const override; // the selected notes', from the first's start to the last's end
    void quantizeSelection() override;
    juce::String getQuantizeName() const override; // with the strength and swing Quantize... was given last

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override; // another theme: its scroll bar's colours, and its instrument button's, in it
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr double scrollPerNotch = 48.0;
    double rowsWheelRest = 0.0, scrollWheelRest = 0.0; // the parts of a step the wheel has turned the rows' height and scroll by

    bool cancelDrag() override; // Escape: in the notes or the lane under them, whichever it shows

    // Its parts (PianoRollParts.h).
    struct Keys;            // the piano keys, or the drums' names, down the left
    struct NoteGrid;        // the rows of notes
    struct VelocityLane;    // the velocity of each note, under them
    struct ControlLaneView; // or a controller's values, in its place
    struct LaneButton;      // which of them shows
    struct TransformBar;    // a transform's settings, while its preview shows

    // A transform with its settings, as the transform bar edits them.
    struct Transform
    {
        enum class Kind
        {
            quantize,
            legato,
            chop,
            transpose,
            strum,
            humanize,
            arpeggiate
        };

        Kind kind = Kind::quantize;
        QuantizeSettings quantize {}; // its grid and origin are the piano roll's
        double legatoGapBeats = 0.0;
        int transposeSteps = 0;
        bool transposeInKey = true;
        StrumSettings strum {};
        HumanizeSettings humanize {};
        ArpeggioSettings arpeggio {}; // its step is the grid's
    };

    ProjectContext& context;
    SongSelection& selection;  // the context's
    int shownProject = -1;     // the project shown (ProjectContext::getProjectNumber()), to tell another one from the same one
    std::vector<ClipID> clips; // shown
    ClipID targetClip;         // where new notes go, if the click isn't in a clip
    TrackID emptyTrack;        // a track shown without clips; a note drawn makes one

    // The rows: one per pitch, high to low, or in drum mode one per drum.
    int rowHeight = 14;
    int scrollY = 0;
    bool drumMode = false;
    std::vector<DrumRow> drumRows;

    // What new notes get: the length and velocity used last, a chord, and the song's key.
    BeatDuration lastLength = BeatDuration::fromBeats (0.25);
    int lastVelocity = 100;
    std::optional<ChordType> chordStamp;
    bool snapToKey = false, showGhostNotes = true;
    Transform transform;        // the transform the bar shows, with the settings used last
    Transform transformBefore;  // as they were when the bar opened: closing it without Apply puts them back
    bool transformOpen = false; // while its preview shows

    // Each clip's view as it was left, by the clip's ID, to open it there again (forgotten when another project opens). Linked
    // clips (aliases), which show the same notes, share it: left in one, each opens as that one was left (the owner,
    // 2026-11-13), counted from its own bar 1.
    struct ClipView
    {
        double pixelsPerBeat = 0.0;
        double offsetBeats = 0.0; // the view's start, from bar 1
        int scrollY = 0;
        std::optional<ControlLane> lane; // what the lane under the notes showed: none for the velocities
    };

    std::map<ClipID, ClipView> clipViews;

    // Down the left (layOutSettings()), in a view that scrolls where the piano roll is too short for them, as the audio clip
    // editor in the same tab shows its clip's settings down its left: which clips show and where, the track's instrument and
    // key, what a click adds, the chord stamp and ghost notes. The notes' menu, with the transforms, is the More menu of the
    // area's tab row (TimelineEditor::getToolRow()). Until 2026-11-13 they were in a toolbar along the top, which the default
    // window was too narrow for.
    juce::Label clipLabel; // which clips are shown, and where they are in the song
    juce::TextButton instrumentButton, keyButton, nextNoteButton, chordButton, ghostButton { "Ghost notes" };
    juce::Component settingsContent;
    juce::Viewport settingsView;
    void layOutSettings();

    // The velocity lane's height, while it isn't folded, and its chevron, at its top left; and where its row is, as laid out last.
    int velocityHeight = defaultVelocityHeight;
    bool velocityFolded = false;
    juce::Rectangle<int> velocityRow;
    IconButton velocityFoldButton { "Fold the lane", Icon::expanded };
    struct VelocityEdge; // the line between the notes and the velocities, dragged to size the lane
    std::unique_ptr<VelocityEdge> velocityEdge;
    void setVelocityFolded (bool);
    int clampVelocityHeight (int) const; // tall enough to draw on, and leaving the notes some room
    void velocityLaneChanged();          // tells the settings (onVelocityLaneChanged)

    std::unique_ptr<Keys> keys;
    std::unique_ptr<NoteGrid> grid;
    std::unique_ptr<VelocityLane> velocityLane;
    std::unique_ptr<ControlLaneView> controlLane;
    std::unique_ptr<LaneButton> laneButton;
    std::unique_ptr<TransformBar> transformBar;

    // The lane shown (getLaneShown()), and the lanes the clips shown hold values of, which the chooser marks while the lane
    // shown isn't one of them (nothing plays unseen). Those are read through the clips' elements, so they are kept with the
    // song's version and the clips shown, as what the lanes draw is, and read again only when one of those changed and the
    // chooser or its menu asks; and the chooser asks again only once a drag in the notes or the lane is over, not at each of
    // its steps (a note dragged in a clip of a long take's values).
    std::optional<ControlLane> laneShown;
    struct LanesHeld
    {
        std::vector<ControlLane> lanes;
        int songVersion = 0;
        std::vector<ClipID> shown;
    };
    std::optional<LanesHeld> lanesHeld;
    const std::vector<ControlLane>& getLanesHeld();
    bool isDraggingInNotesOrLane() const;
    void updateLaneButton(); // its name, its title, its mark and its tooltip; after the drag, during one
    void showLaneMenu();
    TrackID auditionTrack; // the track a note is being auditioned on

    // What the note grid and the velocity lane draw: the clips' notes, their colours and which are selected, read again
    // once the song (ProjectContext::getSongVersion()) or the clips shown changed, and which are selected once the
    // selection did, rather than at each paint, as the playhead repaints a strip of the grid at each frame while playing
    // (docs/archive/performance-audit.md, U5 and section 8). The song's version changes with each change at once, so these
    // are the song's notes as they are now whenever they are asked for: the hover and the selection rectangle find notes
    // among them too, rather than reading every note from the song at each move of the mouse (known bug 122).
    struct DrawnNotes
    {
        std::vector<NoteData> notes;
        std::map<ClipID, juce::Colour> colours;
        std::unordered_set<NoteRef> selected;
    };

    mutable std::optional<DrawnNotes> drawnNotes;
    mutable int drawnSongVersion = 0;
    mutable bool drawnSelectionChanged = false; // which notes are selected, to be read again

    //==============================================================================
    void projectChanged() override; // another project, or none
    void songChanged (const SongChanges&) override;

    // The clips and their notes. New clips open at bar 1 with their notes in view, zoomed to fit or as they were left,
    // unless keepView is set.
    void setClips (const std::vector<ClipID>&, bool keepView = false);
    void rememberView(); // the view of the clip shown, to open it so again
    void showNewClips(); // the view the clips shown were left at, or else zoomed to fit them
    void zoomToClips();  // with room after them, never smaller than the editors' usual zoom
    void clipsChanged(); // the notes or the clips moved: repaint, and update the drum rows
    std::vector<NoteClip> getShownClips() const;
    std::vector<NoteData> getAllNotes() const;
    const DrawnNotes& getDrawnNotes() const;                 // as they are now, for painting
    std::unordered_set<NoteRef> getSelectedNoteRefs() const; // the selected notes, if they are these clips'
    std::vector<NoteData> getSelectedNotesHere() const;
    std::vector<NoteRef> getSelectedRefsHere() const;
    void selectNotes (const std::vector<NoteRef>&, bool addToSelection);
    void selectAllNotes();
    ClipID getClipForNewNote (BeatPosition) const; // the clip the beat is in, or the target clip
    ClipID makeClipForNewNotes();                  // on a track shown without clips, from the time origin
    void updateTimeOrigin (bool scrollToStart);    // bar 1 at the first clip's content start
    std::optional<Key> getKey() const;             // the track's key, if it has one
    void setKey (const std::optional<Key>&);
    int getRowHeight() const;
    std::vector<NoteData> getPreview() const; // the selected notes as the open transform would make them
    void applyTransform();
    void openTransform (Transform::Kind);
    void closeTransform (bool keepSettings = false); // without keepSettings, as they were when it opened
    juce::String describeTransform() const;

    // The length and velocity new notes get, which its button in the settings shows and its menu chooses.
    void setNextNote (BeatDuration, int velocity);
    juce::String describeNextNote() const; // "1/4 beat, velocity 100"
    void showNextNoteMenu();

    // Rows: in the note grid's coordinates, row 0 at the top. A pitch as its row names it: "C4" (middle C), or a drum's name.
    int getNumRows() const;
    int getRowForPitch (int pitch) const; // -1 for a drum without a row
    juce::String describePitch (int pitch) const;
    int getPitchForRow (int row) const;
    int getRowTop (int row) const;
    int getRowAt (int y) const;
    void scrollToNotes(); // so that the clips' notes (or middle C) are in view
    void updateVerticalScrollBar();
    juce::Rectangle<float> getNoteBounds (const NoteData&) const;

    // Its bounds on the screen's pixels, so many to one of the piano roll's, so that its edges are sharp: its ends where the
    // time line's edges are (TimelineView::beatToEdge()), which move with the song as it scrolls, its top and foot rounded.
    juce::Rectangle<float> getNoteShape (const NoteData&, float scale) const;

    // Playing notes as they are placed or clicked, on their track's instrument: a note's on its MIDI channel, a new note or
    // a key on the first.
    void audition (ClipID, int pitch, int velocity, int channel = 1);
    void audition (ClipID, const std::vector<int>& pitches, int velocity, int channel = 1);
    void stopAudition();

    void updateSettings();    // what the settings down the left say: the clips, the instrument, the key, the next note, the chord
    void instrumentClicked(); // opens or replaces the track's instrument, or chooses one
    void showChordMenu();
    void showKeyMenu();
    void setDrumMode (bool);

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;       // the notes' and the velocities'
    std::vector<OverviewMark> getOverviewMarks() const override; // the clips' notes, at their pitches
    BeatRange getContentRange() const override;
    void timelineChanged() override;
    std::optional<double> getLinkedPlayheadBeat (double songBeat) const override; // a linked clip of a shown one plays
    void toolChanged() override;
    void gridChanged() override;
    void verticalScrollBarMoved (double newRangeStart) override;
    bool scrollVerticallyBy (int pixels) override;
    juce::String describeToolHere (Tool) const override;
    static juce::String describeNoteTool (Tool); // what the tool does to notes (PianoRollNotes.cpp)

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
} // namespace phazite::ui
