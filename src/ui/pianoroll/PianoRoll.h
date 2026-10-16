// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll (docs/design.md, section 7; roadmap step 1.3): the notes of the MIDI clips selected in the
// playlist, several at once, each in its track's color, or of the selected track's clip nearest the playhead.
// Bar 1 is where the (first) clip starts, as in FL Studio and Live. FL Studio's mouse model: a click adds a
// note with the last length and velocity, a right-click deletes, dragging a note moves it and dragging its
// edge resizes it. It has the playlist's tools and grid (TimelineEditor), a velocity lane, ghost notes from
// other tracks, shading for notes outside the track's key, chord stamps, drum mode for drum tracks, and
// transforms that show what they would do before they are applied.
#pragma once

#include "model/notes/Notes.h"
#include "ui/timeline/TimelineEditor.h"

#include <optional>

namespace phazite::ui
{
class PianoRoll : public TimelineEditor,
                  private juce::ChangeListener,
                  private juce::ValueTree::Listener,
                  private juce::AsyncUpdater,
                  private te::SelectableListener
{
public:
    PianoRoll (CommandBridge&, te::SelectionManager&);
    ~PianoRoll() override;

    // The open project's Edit, or nullptr while none is open.
    void setEdit (te::Edit*);

    // Shows these clips' notes. The piano roll follows the playlist's selection by itself; this is for
    // opening a clip, as double-clicking it in the playlist does.
    void showClips (const juce::Array<te::MidiClip*>&);
    juce::Array<te::MidiClip*> getClips() const; // those still in the song

    // Shows a track's MIDI clip at the playhead, or the one nearest to it. Without one, the piano roll shows
    // the track from the bar the playhead is in, and the first note drawn makes a clip there.
    void showTrack (te::AudioTrack&);
    te::AudioTrack* getTrack() const; // the track shown: the first clip's, or the one showTrack() was given

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

    te::SelectionManager& selection;
    juce::ValueTree editState; // listened to for changes to notes and clips
    std::vector<te::SafeSelectable<te::MidiClip>> clips;
    std::unique_ptr<te::SelectedMidiEvents> selectedEvents; // the selection of these clips' notes
    te::SafeSelectable<te::MidiClip> targetClip;            // where new notes go, if the click isn't in a clip
    te::SafeSelectable<te::AudioTrack> emptyTrack;          // a track shown without clips; a note drawn makes one

    // The rows: one per pitch, high to low, or in drum mode one per drum.
    int rowHeight = 14;
    int scrollY = 0;
    bool drumMode = false;
    std::vector<DrumRow> drumRows;

    // What new notes get: the length and velocity used last, a chord, and the song's key.
    te::BeatDuration lastLength = te::BeatDuration::fromBeats (0.25);
    int lastVelocity = 100;
    std::optional<te::Chord::ChordType> chordStamp;
    bool snapToKey = false, showGhostNotes = true;
    Transform transform;        // the transform the bar shows, with the settings used last
    bool transformOpen = false; // while its preview shows

    juce::TextButton instrumentButton, chordButton, keyButton, ghostButton { "Ghosts" }, transformButton { "Transform" };
    juce::Label clipLabel; // which clips are shown, and where they are in the song

    std::unique_ptr<Keys> keys;
    std::unique_ptr<NoteGrid> grid;
    std::unique_ptr<VelocityLane> velocityLane;
    std::unique_ptr<TransformBar> transformBar;
    te::SafeSelectable<te::AudioTrack> auditionTrack; // the track a note is being auditioned on

    //==============================================================================
    // The clips and their notes. New clips open at bar 1 with their notes in view, unless keepView is set.
    void setClips (const juce::Array<te::MidiClip*>&, bool keepView = false);
    void clipsChanged(); // the notes or the clips moved: repaint, and update the drum rows
    std::vector<ClipNote> getAllNotes() const;
    std::vector<ClipNote> getSelectedNotesHere() const; // the selected notes, if they are these clips'
    bool isSelected (const te::MidiNote&) const;
    void selectNotes (const juce::Array<te::MidiNote*>&, bool addToSelection);
    void selectAllNotes();
    te::MidiClip* getClipForNewNote (te::BeatPosition) const; // the clip the beat is in, or the target clip
    te::MidiClip* makeClipForNewNotes();                      // on a track shown without clips, from the time origin
    void updateTimeOrigin (bool scrollToStart);               // bar 1 at the first clip's content start
    std::optional<Key> getKey() const;                        // the track's key, if it has one
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
    void audition (te::MidiClip&, int pitch, int velocity);
    void audition (te::MidiClip&, const std::vector<int>& pitches, int velocity);
    void stopAudition();

    void updateToolbar();
    void instrumentClicked(); // opens or replaces the track's instrument, or chooses one
    void showChordMenu();
    void showKeyMenu();
    void showTransformMenu();
    void setDrumMode (bool);

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
    te::BeatRange getContentRange() const override;
    void timelineChanged() override;
    std::optional<double> getLinkedPlayheadBeat (double songBeat) const override; // a linked clip of a shown one plays
    void toolChanged() override;
    void gridChanged() override;
    void verticalScrollBarMoved (double newRangeStart) override;
    bool scrollVerticallyBy (int pixels) override;
    juce::String describeToolHere (Tool) const override;
    static juce::String describeNoteTool (Tool); // what the tool does to notes (PianoRollNotes.cpp)

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void selectableObjectChanged (te::Selectable*) override {}
    void selectableObjectAboutToBeDeleted (te::Selectable*) override; // a clip shown here
    void handleAsyncUpdate() override;                                // notes or clips changed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
} // namespace phazite::ui
