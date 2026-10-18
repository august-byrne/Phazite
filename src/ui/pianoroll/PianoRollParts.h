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
// Down the left: the piano keys, which play their note while held, or in drum mode the drums' names, which
// can be dragged into another order or hidden.
struct PianoRoll::Keys : public juce::Component
{
    explicit Keys (PianoRoll&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    PianoRoll& roll;
    int playingPitch = -1; // the key held down
    int draggedRow = -1;   // drum mode: the row being moved
    int dropRow = -1;      // and where it would go
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

    std::optional<NoteData> getNoteAt (juce::Point<float>) const;
    Zone getZone (const NoteData&, juce::Point<float>) const;
    ClipID getGhostClipAt (juce::Point<float>) const;
    std::vector<NoteData> getGhostNotes() const; // other tracks' notes in view

    void paintRows (juce::Graphics&, juce::Rectangle<int> area);
    void paintChordTones (juce::Graphics&, juce::Rectangle<int> area); // the chord lane's chords' notes, in their rows
    void paintNotes (juce::Graphics&);
    void beginDrag (const juce::MouseEvent&);
    juce::Point<int> getGestureStart() const; // where the mouse went down, in the grid as it is now
    void dragNotes (const juce::MouseEvent&);
    void addNoteAt (const juce::MouseEvent&); // or a chord, with a chord stamp chosen
    void eraseOrMuteAt (juce::Point<float>, bool erase);
    void paintAt (const juce::MouseEvent&);
    void showMenu();
    void updateHint (juce::Point<float>);
    void cancelGesture(); // when the tool changes mid-drag
};

//==============================================================================
// What the mouse is doing in the grid, from the moment it goes down until it goes up.
struct PianoRoll::NoteGrid::Gesture : public TimelineGesture
{
    enum class Kind
    {
        pending, // not dragging yet
        move,
        resizeStart,
        resizeEnd,
        select, // a rectangle
        paint,
        erase,
        mute,
        zoom
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
    int paintPitch = -1;
    int auditionedPitch = -1;
};

//==============================================================================
// Under the notes: each note's velocity as a bar. Dragging across the lane sets the velocities of the notes
// under the mouse, of the selected ones if any are there.
struct PianoRoll::VelocityLane : public juce::Component
{
    explicit VelocityLane (PianoRoll&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    PianoRoll& roll;
    bool dragging = false;
    std::unique_ptr<SongGesture> step; // a drag's undo step, from mouse down to mouse up

    int velocityAt (float y) const;
    void setVelocitiesAt (const juce::MouseEvent&);
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
