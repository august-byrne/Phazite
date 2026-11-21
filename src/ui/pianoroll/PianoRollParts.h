// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's parts, for its own .cpp files: PianoRoll.cpp is the view and what it shows, PianoRollRows.cpp
// lays out the rows, PianoRollCommands.cpp holds the transforms, the settings' menus and the commands, PianoRollNotes.cpp
// is the note grid and PianoRollNoteTools.cpp what the mouse does in it, PianoRollLanes.cpp the keys, the velocity lane and
// the transform bar, and PianoRollControlLane.cpp the controller lane and its chooser.
#pragma once

#include "PianoRoll.h"
#include "ui/components/Controls.h"
#include "ui/timeline/TimelineGesture.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
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
    ClipID getGhostClipAt (juce::Point<float>);            // of the ghosts painted
    std::vector<NoteData> getGhostNotes (BeatRange) const; // in a part of the song, as they are now
    const DrawnGhosts& getDrawnGhosts();                   // as they are now, for painting, and for finding one

    void paintRows (juce::Graphics&, juce::Rectangle<int> area);
    void paintChordTones (juce::Graphics&, juce::Rectangle<int> area); // the chord lane's chords' notes, in their rows
    void paintNotes (juce::Graphics&);
    void beginDrag (const juce::MouseEvent&);
    juce::Point<int> getGestureStart() const; // where the mouse went down, in the grid as it is now
    void dragNotes (const juce::MouseEvent&);
    void setDraggedNotes (const std::vector<NoteData>&); // what a drag moves or resizes, as it is now
    void copyDraggedNotes();                             // Ctrl+drag: the copies, which move from here on
    void removeCopies();                                 // and back where they began, the copies go
    bool endMoveThatWentNowhere();                       // as the mouse goes up: whether the move left its notes where they began
    void clickNote (NoteRef, juce::ModifierKeys);        // a click on a note that waited for the mouse to go up
    juce::String describeDrag() const;                   // where the drag has the note under the pointer, for the hint bar: "E4 at 3.2.1"
    void addNoteAt (const juce::MouseEvent&);            // or a chord, with a chord stamp chosen
    void eraseOrMuteAt (juce::Point<float>, bool erase);
    void beginPaint (const juce::MouseEvent&);
    void paintAt (const juce::MouseEvent&);
    void showMenu();                      // the notes' menu, at the pointer (Shift+right-click)
    void addMenuItems (juce::PopupMenu&); // what it holds, which the tab row's More menu shows too (TimelineEditor::getToolRow())
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
        std::optional<BeatPosition> latestStart {}; // its clip's NoteClip::latestNoteStart

        // How far back its start can go: to its clip's start, and not at all if it starts before it already (a note that a
        // split or a trim cut), which then stays where it is rather than being pulled to the clip's start.
        BeatDuration getRoomBefore() const noexcept { return std::min (BeatDuration(), clipStart - start); }

        // And how far on, where the next clip on the track keeps its clip from growing to hold it: to where its clip still
        // plays it, so that a drag can't leave it in its clip unplayed, where the piano roll wouldn't show it. None while
        // its clip can grow.
        std::optional<BeatDuration> getRoomAfter() const noexcept
        {
            return latestStart.has_value() ? std::optional (std::max (BeatDuration(), *latestStart - start)) : std::nullopt;
        }
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
    NoteRef pressed {};                         // the note the mouse went down on, also while its copy is what moves
    bool waitedForClick = false;                // the press's click waited for the mouse to go up (clickOnMouseUp, as it was)
    bool stepIsTheDrags = false;                // the drag began the undo step: one that moves nothing in the end is taken back
    bool copies = false;                        // Ctrl+drag: copies of the notes move, made once the drag moves them
    std::vector<NoteRef> copied;                // the copies, once made,
    std::vector<NoteRef> selectionBeforeCopies; // and what was selected until then,
    std::vector<Original> originalsOfCopies;    // and the notes they copy, which move again if the copies go
    Original anchorOfCopies;
    ClipEnds clipEndsAtPress;                   // where the shown clips ended as the mouse went down, which each step puts back
    BeatDuration movedBy;                       // a move: how far the notes are from where they began,
    int movedRows = 0;                          // along and in rows
    std::unordered_set<NoteRef> selectedBefore; // a rectangle with Ctrl adds to these
    std::vector<NoteRef> selectionBefore;       // the notes here selected when the mouse went down, for a click that slipped
    SelectionAtPress selectionAtPress;          // and all that was selected, whatever its kind, for Escape
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
// or lowers those under the pointer, a burst of it one undo step (VelocityWheel), and elsewhere scrolls.
struct PianoRoll::VelocityLane : public juce::Component,
                                 private juce::Timer
{
    explicit VelocityLane (PianoRoll&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override; // ends the wheel's burst
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void timerCallback() override; // the wheel has rested: its burst ends

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
    double wheelRest = 0.0;                           // the part of a notch the wheel has turned by and not yet changed velocities for
    VelocityWheel wheel { roll.context };             // the wheel's burst, one undo step
    static constexpr int wheelRestMilliseconds = 500; // as long as the wheel rests before its burst ends

    bool isDragging() const noexcept { return drag != Drag::none; }
    void cancelDrag(); // Escape: as it was before the mouse went down
    void endDrag();    // another project opened: what it did stays, in the song it did it in
    void endBurst();   // the wheel's: what it changed is one step

    int velocityAt (float y) const;
    std::vector<NoteRef> getNotesAt (float x) const; // whose bars the pointer is on: the selected ones, if it is on any
    void setVelocitiesAt (const juce::MouseEvent&);
    void shiftVelocities (const juce::MouseEvent&);
    void drawRamp (const juce::MouseEvent&);
};

//==============================================================================
// The chooser of what the lane under the notes shows (PianoRoll::getLaneMenu()), beside its chevron: the lane's name, cut
// where the room is short and whole in its tooltip, with a mark while the clips shown hold values the lane doesn't show,
// which its tooltip names (nothing plays unseen).
struct PianoRoll::LaneButton : public juce::TextButton
{
    LaneButton();
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    bool marked = false;
};

//==============================================================================
// In the velocity lane's place while a controller's lane is chosen (PianoRoll::showLane()): its values as MIDI has them,
// steps that each hold until the next, a stepped line with what is under it filled in the clip's colour (the pitch bend's
// from its middle, up and down), a dot at each value's start where they are far enough apart to see; a program change as a
// line with its number, and a note's pressure as a stem for each value, as the velocities show (they overlap). Where no
// clip shown plays, it shows what the note grid shows there, and nothing is drawn.
//
// The mouse, as in the velocity lane, whatever the tool but the Delete tool: a drag draws, the values across what the
// pointer passed replaced by those under it as it passed, one at each step of the grid with Snap on, else one at each pixel
// (with Alt too), a click one value; a right drag draws a straight ramp, shown as it goes and put in place of what is between
// its ends as the button comes up; and the Delete tool's drag deletes the values it passes over. Each a step of its own,
// "Draw Sustain Pedal" or "Delete Sustain Pedal", which Escape takes back. A value goes into the clip shown that plays where
// it is (the first shown, where two do), and its aliases. The wheel scrolls.
struct PianoRoll::ControlLaneView : public juce::Component
{
    explicit ControlLaneView (PianoRoll&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    enum class Drag
    {
        none,
        draw,
        ramp,   // with the right button
        erase,  // the Delete tool
        nothing // a drag that changes nothing: a note's pressure drawn
    };

    // What it draws, read again once the song (ProjectContext::getSongVersion()), the clips shown or the lane changed, not at
    // each paint (PianoRoll::DrawnNotes says why): the lane's values, clip by clip, each clip's in time order, and the clips.
    struct Drawn
    {
        struct Clip
        {
            ClipID id;
            BeatRange range;
            juce::Colour colour;
            size_t first = 0, end = 0; // its values' place among the points
        };

        std::vector<ControlPoint> points;
        std::vector<Clip> clips;
        int songVersion = 0;
        std::vector<ClipID> shown;
        ControlLane lane;
    };

    PianoRoll& roll;
    std::optional<Drawn> drawn;

    Drag drag = Drag::none;
    std::unique_ptr<SongGesture> step;       // a drag's undo step, from mouse down to mouse up
    ControlLane dragged;                     // the lane it began in
    std::vector<ControlPoint> valuesAtPress; // its values in the clips shown as it began: a drag that leaves them so is no step
    std::map<std::int64_t, int> drawnCells;  // a draw: the value at the start of each cell the pointer passed, by its number
    BeatPosition cellOrigin;                 // where cell 0 starts, and how long a cell is, as the draw began: a step of the
    BeatDuration cellLength;                 // grid, or a pixel (no shorter than a value's gap), whatever the view does after
    int songVersionDrawn = -1;               // the song's version after a draw's last step, which repainted what it changed,
    int songVersionHeard = -1;               // and as the piano roll last heard the song change
    std::optional<BeatRange> passed;         // the cells passed so far, of a draw or a delete
    juce::Point<float> lastPosition;         // where the pointer was at the drag's last event
    BeatPosition rampFrom, rampTo;           // a ramp's ends, snapped,
    int rampFromValue = 0, rampToValue = 0;  // and its values there

    const Drawn& getDrawn();
    bool isDragging() const noexcept { return drag != Drag::none; }
    void cancelDrag(); // Escape: as it was before the mouse went down
    void endDrag();    // another project opened: what it did stays, in the song it did it in

    // A change of the song heard (PianoRoll::clipsChanged()), at its version: whether the lane repaints all of itself for it,
    // or a draw's steps made every change since the last one heard and repainted the columns they changed themselves.
    bool needsRepaintFor (int songVersion) noexcept
    {
        songVersionHeard = songVersion;
        return ! (drag == Drag::draw && songVersion == songVersionDrawn);
    }

    // The rows its values span, from the highest to none: from the first under the line that sizes the lane, which takes the
    // pointer over its top rows (PianoRoll.cpp: velocityEdgeHeight), so that a click there gives the highest, to two above its
    // foot.
    static constexpr float topRow = 3.0f, footRows = 2.0f;

    // The lane's value at a height in it: the lane's rest (the bend's middle) wherever the rest is within the pixel row, so
    // that a bend can be drawn back to none at any height; and the row the rest is drawn on.
    int valueAt (float y) const;
    float yOf (int value) const;
    int getRestRow() const;
    ClipID getClipAt (BeatPosition); // the clip shown that plays there, the first of two
    BeatRange getCell (BeatPosition, juce::ModifierKeys) const;
    std::int64_t getCellNumber (BeatPosition) const; // a draw's cells, as it began
    BeatPosition getCellStart (std::int64_t) const;

    // The parts of a range where each clip shown is the first that plays: what it draws there.
    std::vector<std::pair<ClipID, BeatRange>> getClipParts (BeatRange);

    void drawTo (const juce::MouseEvent&);             // the cells passed since the last event
    void putCells (BeatRange);                         // the cells drawn there, in place of the clips' values
    void repaintDrawn (BeatRange);                     // the columns a draw's step changed, and those of the value after them
    void eraseTo (const juce::MouseEvent&);            // the cells passed since the last event
    void putRamp();                                    // as the button comes up
    juce::String describeAt (const juce::MouseEvent&); // the hint: "Sustain Pedal 127 at 3.2.1: drag to draw..."
    void paintValues (juce::Graphics&, const Drawn&, const Drawn::Clip&, juce::Rectangle<int> area);
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
