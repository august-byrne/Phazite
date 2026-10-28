// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll (docs/design.md, section 7; roadmap step 1.3): the notes of the MIDI clips selected in the
// playlist, several at once, each in its track's color, or of the selected track's clip nearest the playhead.
// Bar 1 is where the (first) clip starts, as in FL Studio and Live. FL Studio's mouse model: a click adds a
// note with the last length and velocity, a right-click deletes, dragging a note moves it and dragging its
// edge resizes it. It has the playlist's tools and grid (TimelineEditor), a velocity lane, ghost notes from
// other tracks, shading for notes outside the track's key, chord stamps, drum mode for drum tracks, and
// transforms that show what they would do before they are applied. It works by IDs, through the model's
// NotesModel.h, and selects notes through the song's selection.
#pragma once

#include "model/notes/NotesModel.h"
#include "model/project/ProjectContext.h"
#include "ui/timeline/TimelineEditor.h"

#include <map>
#include <optional>
#include <unordered_set>

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

    // The piano roll's commands: select all notes, and the transforms with their settings.
    void addCommands (CommandRegistry&);
    void refreshShortcutTexts() override;

    // TimelineEditor
    bool canUseTool (Tool) const override; // all but Slip
    bool canNudgeSelection() const override;
    void nudgeSelection (int direction) override;
    bool canQuantizeSelection() const override;
    void quantizeSelection() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr double scrollPerNotch = 48.0;

    // Its parts (PianoRollParts.h).
    struct Keys;         // the piano keys, or the drums' names, down the left
    struct NoteGrid;     // the rows of notes
    struct VelocityLane; // the velocity of each note, under them
    struct TransformBar; // a transform's settings, while its preview shows

    // A transform with its settings, as the transform bar edits them.
    struct Transform
    {
        enum class Kind
        {
            quantize,
            legato,
            chop,
            transpose
        };

        Kind kind = Kind::quantize;
        QuantizeSettings quantize {};
        double legatoGapBeats = 0.0;
        int transposeSteps = 0;
        bool transposeInKey = true;
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
    bool transformOpen = false; // while its preview shows

    juce::TextButton instrumentButton, chordButton, keyButton, ghostButton { "Ghosts" }, transformButton { "Transform" };
    juce::Label clipLabel; // which clips are shown, and where they are in the song

    std::unique_ptr<Keys> keys;
    std::unique_ptr<NoteGrid> grid;
    std::unique_ptr<VelocityLane> velocityLane;
    std::unique_ptr<TransformBar> transformBar;
    TrackID auditionTrack; // the track a note is being auditioned on

    // What the note grid and the velocity lane draw: the clips' notes, their colours and which are selected, read again
    // once the song (ProjectContext::getSongVersion()), the clips shown or the selection changed, rather than at each
    // paint, as the playhead repaints a strip of the grid at each frame while playing (docs/performance-audit.md, U5 and
    // section 8). Only for painting: the tools read the song as it is (getAllNotes()).
    struct DrawnNotes
    {
        std::vector<NoteData> notes;
        std::map<ClipID, juce::Colour> colours;
        std::unordered_set<NoteRef> selected;
    };

    mutable std::optional<DrawnNotes> drawnNotes;
    mutable int drawnSongVersion = 0;

    //==============================================================================
    void projectChanged() override; // another project, or none
    void songChanged (const SongChanges&) override;

    // The clips and their notes. New clips open at bar 1 with their notes in view, unless keepView is set.
    void setClips (const std::vector<ClipID>&, bool keepView = false);
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
    void closeTransform();
    juce::String describeTransform() const;

    // Rows: in the note grid's coordinates, row 0 at the top.
    int getNumRows() const;
    int getRowForPitch (int pitch) const; // -1 for a drum without a row
    int getPitchForRow (int row) const;
    int getRowTop (int row) const;
    int getRowAt (int y) const;
    void setScrollY (int);
    void scrollToNotes(); // so that the clips' notes (or middle C) are in view
    void updateVerticalScrollBar();
    juce::Rectangle<float> getNoteBounds (const NoteData&) const;

    // Playing notes as they are placed or clicked, on their track's instrument.
    void audition (ClipID, int pitch, int velocity);
    void audition (ClipID, const std::vector<int>& pitches, int velocity);
    void stopAudition();

    void updateToolbar();
    void instrumentClicked(); // opens or replaces the track's instrument, or chooses one
    void showChordMenu();
    void showKeyMenu();
    void showTransformMenu();
    void setDrumMode (bool);

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
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
