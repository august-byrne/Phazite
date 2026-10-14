// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's parts, for its own .cpp files: PianoRoll.cpp lays them out, PianoRollNotes.cpp is the note
// grid and PianoRollLanes.cpp the keys, the velocity lane and the transform bar.
#pragma once

#include "PianoRoll.h"
#include "ui/components/Controls.h"

namespace phazite::ui
{
// The color a clip's notes are drawn in: the clip's, or its track's.
juce::Colour getNoteColour (te::MidiClip&);

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

    std::optional<ClipNote> getNoteAt (juce::Point<float>) const;
    Zone getZone (const NoteData&, juce::Point<float>) const;
    te::MidiClip* getGhostClipAt (juce::Point<float>) const;
    std::vector<std::pair<te::MidiClip*, NoteData>> getGhostNotes() const; // other tracks' notes in view

    void paintRows (juce::Graphics&, juce::Rectangle<int> area);
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
