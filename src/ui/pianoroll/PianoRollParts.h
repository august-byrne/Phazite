// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's parts, for its own .cpp files: PianoRoll.cpp is the view and what it shows, PianoRollRows.cpp
// lays out the rows, PianoRollCommands.cpp holds the transforms, the toolbar's menus and the commands, PianoRollNotes.cpp
// is the note grid and PianoRollNoteTools.cpp what the mouse does in it, and PianoRollLanes.cpp the keys, the velocity
// lane and the transform bar.
#pragma once

#include "PianoRoll.h"
#include "ui/components/Controls.h"
#include "ui/timeline/TimelineGesture.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <map>

namespace phazite::ui
{
// The colors the notes of these clips are drawn in: each clip's, its track's.
std::map<ClipID, juce::Colour> getNoteColours (const ProjectContext&, const std::vector<NoteData>&);

//==============================================================================
// Down the left: the piano keys, which play their note while held and select its notes, as in Live and Logic Pro, or
// in drum mode the drums' names, which can be dragged into another order or hidden.
struct PianoRoll::Keys : public juce::Component
{
    explicit Keys (PianoRoll&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    PianoRoll& roll;
    int playingPitch = -1;                // the key held down
    int pressedPitch = -1;                // where the press began: a drag selects the notes of the pitches between
    bool addsToSelection = false;         // Ctrl or Shift was held: to the notes selected before
    std::vector<NoteRef> selectionBefore; // with it
    int draggedRow = -1;                  // drum mode: the row being moved
    int dropRow = -1;                     // and where it would go

    void selectPitches (int from, int to); // the shown notes of these pitches and those between
};

//==============================================================================
// The rows of notes. The hint bar shows what the mouse does where it is.
struct PianoRoll::NoteGrid : public juce::Component
{
    explicit NoteGrid (PianoRoll&);
    ~NoteGrid() override;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    struct Gesture;
    enum class Zone
    {
        body,
        start, // the left edge
        end    // the right edge
    };

    PianoRoll& roll;
    std::unique_ptr<Gesture> gesture; // from mouse down to mouse up
    juce::Rectangle<int> rubberBand;  // the Select and Zoom tools' rectangle

    // The other tracks' notes in view, with their colours, as they were painted last: read again once the song, the view,
    // the clips shown or the ghosts' switch changed (PianoRoll::DrawnNotes says why).
    struct DrawnGhosts
    {
        std::vector<NoteData> notes;
        std::map<ClipID, juce::Colour> colours;
        int songVersion = 0;
        BeatRange range;
        std::vector<ClipID> clips;
        TrackID track; // shown without clips
        bool shown = true;
    };

    std::optional<DrawnGhosts> drawnGhosts;

    std::optional<NoteData> getNoteAt (juce::Point<float>) const;
    std::optional<NoteData> getNoteToEditAt (juce::Point<float>) const; // that, or a short note whose edge is just beside it
    Zone getZone (const NoteData&, juce::Point<float>) const;
    ClipID getGhostClipAt (juce::Point<float>) const;
    std::vector<NoteData> getGhostNotes() const;           // other tracks' notes in view, as they are now
    std::vector<NoteData> getGhostNotes (BeatRange) const; // in a part of the song
    const DrawnGhosts& getDrawnGhosts();                   // as they are now, for painting

    void paintRows (juce::Graphics&, juce::Rectangle<int> area);
    void paintChordTones (juce::Graphics&, juce::Rectangle<int> area); // the chord lane's chords' notes, in their rows
    void paintNotes (juce::Graphics&);
    void beginDrag (const juce::MouseEvent&);
    juce::Point<int> getGestureStart() const; // where the mouse went down, in the grid as it is now
    void dragNotes (const juce::MouseEvent&);
    void addNoteAt (const juce::MouseEvent&); // or a chord, with a chord stamp chosen
    void eraseOrMuteAt (juce::Point<float>, bool erase);
    void beginPaint (const juce::MouseEvent&);
    void paintAt (const juce::MouseEvent&);
    void showMenu (juce::Component* target = nullptr); // the notes' menu, at the pointer or under the toolbar's button
    void updateHint (juce::Point<float>);
    void cancelGesture(); // when the tool changes mid-drag: what it did stays

    // While the mouse is down; Escape takes back what it did, and the selection is as it was before.
    bool isDragging() const noexcept { return gesture != nullptr; }
    void cancelDrag();
};

//==============================================================================
// What the mouse is doing in the grid, from the moment it goes down until it goes up.
struct PianoRoll::NoteGrid::Gesture : public TimelineGesture
{
    enum class Kind
    {
        pending, // not dragging yet
        none,    // a drag that does nothing
        move,
        resizeStart,
        resizeEnd,
        select, // a rectangle
        paint,
        erase,
        mute,
        zoom,
        pan // the middle button: moves the view
    };

    // A note as it was when the drag began, and where its clip starts (no note goes before it).
    struct Original
    {
        NoteRef note;
        BeatPosition start;
        BeatDuration length;
        int pitch = 60;
        BeatPosition clipStart;
    };

    // Which ways the grid scrolls when this drags past its edges, if at all.
    std::optional<TimelineEditor::ScrollDirections> getAutoScrollDirections() const
    {
        switch (kind)
        {
            case Kind::move:
            case Kind::select:
            case Kind::erase:
            case Kind::mute:
                return TimelineEditor::ScrollDirections::both;
            case Kind::resizeStart:
            case Kind::resizeEnd:
            case Kind::paint:
            case Kind::zoom:
                return TimelineEditor::ScrollDirections::sideways;
            case Kind::pending:
            case Kind::none:
            case Kind::pan:
            default:
                return std::nullopt;
        }
    }

    Kind kind = Kind::pending;
    Zone zone = Zone::body;
    NoteRef note {}; // the note the mouse went down on, if any

    std::vector<Original> originals;
    Original anchor;                            // the note the mouse went down on, as it was
    std::unordered_set<NoteRef> selectedBefore; // a rectangle with Ctrl adds to these
    std::vector<NoteRef> selectionBefore;       // what was selected when the mouse went down, for Escape
    bool deselectIfNothingDeleted = false;      // a right-click on empty grid
    BeatPosition panViewStart;                  // the middle button: where the view started

    // Paint: the notes (a chord, with a chord stamp) laid at each step of the grid from the step the drag began on, and the
    // steps painted so far, counted from there either way.
    std::vector<int> paintPitches;
    BeatPosition paintOrigin;
    int paintedFrom = 1, paintedTo = 0; // none yet
    bool paintedAny = false;

    int auditionedPitch = -1;
};

//==============================================================================
// Under the notes: each note's velocity as a bar. Dragging across the lane sets the velocities of the notes under the
// mouse, of the selected ones if any are there; a drag from a selected note's bar moves every selected note's velocity by
// as much, as Logic Pro's Velocity tool does; a right drag draws a straight ramp, as in FL Studio; and the wheel raises
// or lowers them.
struct PianoRoll::VelocityLane : public juce::Component
{
    explicit VelocityLane (PianoRoll&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    enum class Drag
    {
        none,
        draw,  // sets the velocities under the mouse
        shift, // moves every selected note's by as much
        ramp   // a straight line, with the right button
    };

    PianoRoll& roll;
    Drag drag = Drag::none;
    std::unique_ptr<SongGesture> step; // a drag's undo step, from mouse down to mouse up
    std::vector<NoteData> before;      // the notes as they were when it began
    float pressY = 0.0f;
    juce::Point<float> rampStart, rampEnd;

    bool isDragging() const noexcept { return drag != Drag::none; }
    void cancelDrag(); // Escape: as it was before the mouse went down

    int velocityAt (float y) const;
    std::vector<NoteRef> getNotesAt (float x) const; // whose bars the pointer is on: the selected ones, if it is on any
    void setVelocitiesAt (const juce::MouseEvent&);
    void shiftVelocities (const juce::MouseEvent&);
    void drawRamp (const juce::MouseEvent&);
};

//==============================================================================
// The settings of the transform being previewed, with Apply and Cancel.
struct PianoRoll::TransformBar : public juce::Component
{
    explicit TransformBar (PianoRoll&);

    void show (const Transform&); // sets the controls from the transform
    void paint (juce::Graphics&) override;
    void resized() override;

    PianoRoll& roll;
    juce::Label title, info;
    ValueSlider first { juce::Slider::LinearHorizontal, "Amount", 0.0 }, second { juce::Slider::LinearHorizontal, "Amount", 0.0 };
    juce::ToggleButton option;
    juce::TextButton applyButton { "Apply" }, cancelButton { "Cancel" };

    void controlsChanged(); // puts the controls' values into the transform
};
} // namespace phazite::ui
