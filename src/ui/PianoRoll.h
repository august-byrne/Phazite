// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll (docs/design.md, section 7; roadmap step 1.3): the notes of the MIDI clips selected in the
// playlist, several at once, each in its clip's color. FL Studio's mouse model: a click adds a note with
// the last length and velocity, a right-click deletes, dragging a note moves it and dragging its edge
// resizes it. It has the playlist's tools and grid (TimelineEditor), a velocity lane, ghost notes from other
// tracks, shading for notes outside the song's key, chord stamps, drum mode, and transforms that show what
// they would do before they are applied.
#pragma once

#include "TimelineEditor.h"
#include "model/Notes.h"

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
    PianoRoll (CommandRegistry&, te::SelectionManager&);
    ~PianoRoll() override;

    // The open project's Edit, or nullptr while none is open.
    void setEdit (te::Edit*);

    // Shows these clips' notes. The piano roll follows the playlist's selection by itself; this is for
    // opening a clip, as double-clicking it in the playlist does.
    void showClips (const juce::Array<te::MidiClip*>&);
    juce::Array<te::MidiClip*> getClips() const; // those still in the song

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

    juce::TextButton chordButton, keyButton, drumButton { "Drums" }, ghostButton { "Ghosts" }, transformButton { "Transform" };
    juce::Label clipLabel; // which clips are shown

    std::unique_ptr<Keys> keys;
    std::unique_ptr<NoteGrid> grid;
    std::unique_ptr<VelocityLane> velocityLane;
    std::unique_ptr<TransformBar> transformBar;
    te::SafeSelectable<te::AudioTrack> auditionTrack; // the track a note is being auditioned on

    //==============================================================================
    // The clips and their notes.
    void setClips (const juce::Array<te::MidiClip*>&);
    void clipsChanged(); // the notes or the clips moved: repaint, and update the drum rows
    std::vector<ClipNote> getAllNotes() const;
    std::vector<ClipNote> getSelectedNotesHere() const; // the selected notes, if they are these clips'
    bool isSelected (const te::MidiNote&) const;
    void selectNotes (const juce::Array<te::MidiNote*>&, bool addToSelection);
    void selectAllNotes();
    te::MidiClip* getClipForNewNote (te::BeatPosition) const; // the clip the beat is in, or the target clip
    Key getKey() const;                                       // the song's key where the clips start
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
    void showChordMenu();
    void showKeyMenu();
    void showTransformMenu();
    void setDrumMode (bool);

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
    te::BeatRange getContentRange() const override;
    void timelineChanged() override;
    void toolChanged() override;
    void gridChanged() override;
    void verticalScrollBarMoved (double newRangeStart) override;
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
