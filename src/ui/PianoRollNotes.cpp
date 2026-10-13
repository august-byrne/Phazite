// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's note grid: drawing the rows and notes, and what the mouse does with the tools.
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "PianoRollParts.h"
#include "model/Clips.h"
#include "model/Tracks.h"

#include <map>

namespace phazite::ui
{
namespace
{
    constexpr int dragThreshold = 4;
    constexpr float noteCorner = 2.0f;

    const juce::String
        emptyHint ("Click to add a note with the last length and velocity, and drag to move it; Ctrl+drag selects notes in a "
                   "rectangle. Right-click deletes, Shift+right-click opens the menu. Ctrl+wheel zooms, Ctrl+Shift+wheel "
                   "makes the rows taller, Alt+wheel changes the selected notes' velocity.");
    const juce::String
        bodyHint ("Drag to move the note, and the selected notes with it; Shift+drag copies them, Alt places them off the grid. "
                  "Ctrl+click adds it to the selection; right-click deletes it.");
    const juce::String endHint ("Drag to make the note longer or shorter, and the selected notes with it. Hold Alt to leave the grid.");
    const juce::String startHint ("Drag to move the note's start, keeping its end. Hold Alt to leave the grid.");
    const juce::String noClipsHint ("Select a MIDI clip or a track in the playlist, or double-click a clip, to edit its notes here.");
    const juce::String emptyTrackHint ("This track has no MIDI clip yet. Draw a note to start one here.");

    juce::Path makeDiamond (juce::Rectangle<float> area)
    {
        juce::Path diamond;
        diamond.addQuadrilateral (area.getCentreX(), area.getY(), area.getRight(), area.getCentreY(), area.getCentreX(), area.getBottom(),
                                  area.getX(), area.getCentreY());
        return diamond;
    }
} // namespace

//==============================================================================
juce::Colour getNoteColour (te::MidiClip& clip)
{
    auto* track = clip.getTrack();
    const auto row = track != nullptr ? getPlaylistTracks (clip.edit).indexOf (track) : 0;
    return getClipColour (clip, track != nullptr ? getTrackColour (*track, row) : juce::Colours::grey);
}

//==============================================================================
juce::String PianoRoll::describeNoteTool (Tool toolToDescribe)
{
    switch (toolToDescribe)
    {
        case Tool::paint:
            return "Drag along a row to lay notes one after another, a step of the grid each.";
        case Tool::slice:
            return "Click a note to split it there. Hold Alt to split off the grid.";
        case Tool::mute:
            return "Click a note to mute or unmute it; drag to do it to several.";
        case Tool::erase:
            return "Click a note to delete it; drag over notes to delete them all.";
        case Tool::select:
            return "Drag to select the notes in a rectangle; with Ctrl, add them to the selection.";
        case Tool::zoom:
            return "Click to zoom in, Alt-click to zoom out, or drag across the time to zoom to.";
        case Tool::smart:
        case Tool::draw:
        case Tool::slip:
        default:
            return emptyHint;
    }
}

juce::String PianoRoll::describeToolHere (Tool toolToDescribe) const
{
    return describeNoteTool (toolToDescribe);
}

//==============================================================================
// What the mouse is doing in the grid, from the moment it goes down until it goes up.
struct PianoRoll::NoteGrid::Gesture
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

    // A note as it was when the drag began, found again by its state (it may be moved, not replaced).
    struct Original
    {
        te::SafeSelectable<te::MidiClip> clip;
        juce::ValueTree state;
        te::BeatPosition start;
        te::BeatDuration length;
        int pitch = 60;
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
    te::SafeSelectable<te::MidiClip> clip; // the note the mouse went down on, if any
    juce::ValueTree noteState;
    te::BeatPosition startBeat;
    juce::Point<int> startPosition;
    int startScrollY = 0;        // how far the rows had scrolled then
    bool clickOnMouseUp = false; // a click on a selected note selects it alone, unless it becomes a drag
    bool added = false;          // the note was added by this click: a drag that follows is the same undo step

    std::vector<Original> originals;
    Original anchor;                             // the note the mouse went down on, as it was
    std::vector<juce::ValueTree> done;           // Delete and Mute: the notes already done
    std::vector<juce::ValueTree> selectedBefore; // a rectangle with Ctrl adds to these
    te::BeatPosition nextPaintStart;
    int paintPitch = -1;
    int auditionedPitch = -1;
};

//==============================================================================
PianoRoll::NoteGrid::NoteGrid (PianoRoll& r)
    : roll (r)
{
    setTitle ("Notes");
    setDescription (emptyHint);
    setWantsKeyboardFocus (true); // a click here gives the piano roll the keys (and makes it the editor the tools act on)
}

PianoRoll::NoteGrid::~NoteGrid() = default;

//==============================================================================
std::optional<ClipNote> PianoRoll::NoteGrid::getNoteAt (juce::Point<float> position) const
{
    // The last drawn is on top.
    const auto notes = roll.getAllNotes();

    for (auto it = notes.rbegin(); it != notes.rend(); ++it)
        if (roll.getNoteBounds (getNoteData (*it->clip, *it->note)).contains (position))
            return *it;

    return {};
}

PianoRoll::NoteGrid::Zone PianoRoll::NoteGrid::getZone (const NoteData& note, juce::Point<float> position) const
{
    if (roll.drumMode)
        return Zone::body; // hits have no length to change

    const auto bounds = roll.getNoteBounds (note);
    const auto edge = std::min (5.0f, bounds.getWidth() / 4.0f);

    if (position.x >= bounds.getRight() - edge)
        return Zone::end;

    if (bounds.getWidth() > 16.0f && position.x <= bounds.getX() + 3.0f)
        return Zone::start;

    return Zone::body;
}

std::vector<std::pair<te::MidiClip*, NoteData>> PianoRoll::NoteGrid::getGhostNotes() const
{
    std::vector<std::pair<te::MidiClip*, NoteData>> ghosts;
    auto* ed = roll.getEdit();

    if (ed == nullptr || ! roll.showGhostNotes)
        return ghosts;

    // The other tracks' MIDI clips that are in view.
    const auto shown = roll.getClips();
    const auto& timeline = roll.getView();
    const te::BeatRange visible (timeline.xToBeat (0), timeline.xToBeat (getWidth()));
    juce::Array<te::Track*> shownTracks { roll.getTrack() };

    for (auto* clip : shown)
        shownTracks.addIfNotAlreadyThere (clip->getTrack());

    for (auto* track : te::getAudioTracks (*ed))
    {
        if (shownTracks.contains (track))
            continue;

        for (auto* c : track->getClips())
            if (auto* midi = dynamic_cast<te::MidiClip*> (c); midi != nullptr && midi->getEditBeatRange().overlaps (visible))
                for (auto* note : midi->getSequence().getNotes())
                    ghosts.emplace_back (midi, getNoteData (*midi, *note));
    }

    return ghosts;
}

te::MidiClip* PianoRoll::NoteGrid::getGhostClipAt (juce::Point<float> position) const
{
    for (const auto& [clip, note] : getGhostNotes())
        if (roll.getNoteBounds (note).contains (position))
            return clip;

    return nullptr;
}

//==============================================================================
void PianoRoll::NoteGrid::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (roll.getEdit() == nullptr)
        return;

    const auto area = g.getClipBounds();

    if (roll.getTrack() == nullptr)
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawFittedText (noClipsHint, getLocalBounds().reduced (Space::xl), juce::Justification::centred, 2);
        return;
    }

    paintRows (g, area);

    // Where the clips are: a line in the clip's color at each end, and a tint between.
    const auto& timeline = roll.getView();

    for (auto* clip : roll.getClips())
    {
        const auto left = juce::roundToInt (timeline.beatToX (clip->getStartBeat()));
        const auto right = juce::roundToInt (timeline.beatToX (clip->getEndBeat()));
        const auto colour = getNoteColour (*clip);
        g.setColour (colour.withAlpha (0.06f));
        g.fillRect (left, area.getY(), right - left, area.getHeight());
        g.setColour (colour.withAlpha (0.6f));
        g.fillRect (left, area.getY(), 1, area.getHeight());
        g.fillRect (right - 1, area.getY(), 1, area.getHeight());
    }

    paintGridLines (g, roll, area);

    // A track without clips yet: the first note drawn starts one.
    if (roll.getClips().isEmpty())
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawFittedText (emptyTrackHint, getLocalBounds().reduced (Space::xl), juce::Justification::centred, 2);
    }

    // Other tracks' notes, faintly.
    for (const auto& [clip, note] : getGhostNotes())
    {
        const auto bounds = roll.getNoteBounds (note);

        if (! bounds.isEmpty() && bounds.intersects (area.toFloat()))
        {
            g.setColour (getNoteColour (*clip).withAlpha (0.22f));

            if (roll.drumMode)
                g.fillPath (makeDiamond (bounds));
            else
                g.fillRoundedRectangle (bounds, noteCorner);
        }
    }

    paintNotes (g);

    if (! rubberBand.isEmpty())
    {
        g.setColour (p.accent.withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (p.accent);
        g.drawRect (rubberBand, 1);
    }
}

void PianoRoll::NoteGrid::paintRows (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Rows outside the track's key are darker; without a key, those of the black keys, as on a piano. In drum mode
    // the rows alternate.
    const auto& p = getPalette (*this);
    const auto key = roll.getKey().value_or (Key { 0, te::Scale::major }); // C major has the white keys
    const auto height = roll.getRowHeight();
    const auto first = std::max (0, roll.getRowAt (area.getY()));
    const auto last = roll.getRowAt (area.getBottom() - 1) >= 0 ? roll.getRowAt (area.getBottom() - 1) : roll.getNumRows() - 1;

    for (auto row = first; row <= last; ++row)
    {
        const auto pitch = roll.getPitchForRow (row);
        juce::Rectangle<int> rowArea (area.getX(), roll.getRowTop (row), area.getWidth(), height);
        const auto inKey = roll.drumMode ? row % 2 == 0 : isInKey (pitch, key);

        g.setColour (inKey ? p.surface : p.background);
        g.fillRect (rowArea);

        // A line under each row, stronger under each C.
        const auto isOctave = ! roll.drumMode && pitch % 12 == 0;
        g.setColour (isOctave ? p.border : p.border.withMultipliedAlpha (0.4f));
        g.fillRect (rowArea.removeFromBottom (1));
    }
}

void PianoRoll::NoteGrid::paintNotes (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto area = g.getClipBounds().toFloat();
    const auto preview = roll.getPreview();
    const auto previewing = roll.transformOpen;
    std::map<te::MidiClip*, juce::Colour> colours;

    for (const auto& n : roll.getAllNotes())
    {
        const auto data = getNoteData (*n.clip, *n.note);
        const auto bounds = roll.getNoteBounds (data);

        if (bounds.isEmpty() || ! bounds.intersects (area))
            continue;

        if (! colours.contains (n.clip))
            colours[n.clip] = getNoteColour (*n.clip);

        const auto selected = roll.isSelected (*n.note);
        auto fill = selected ? colours[n.clip].brighter (0.35f) : colours[n.clip];

        if (data.muted)
            fill = fill.withMultipliedSaturation (0.2f).withAlpha (0.45f);

        if (previewing && selected)
            fill = fill.withAlpha (0.3f); // what the transform replaces

        const auto outline = selected ? p.text : colours[n.clip].darker (0.6f);

        if (roll.drumMode)
        {
            const auto diamond = makeDiamond (bounds);
            g.setColour (fill);
            g.fillPath (diamond);
            g.setColour (outline);
            g.strokePath (diamond, juce::PathStrokeType (selected ? 2.0f : 1.0f));
            continue;
        }

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, noteCorner);
        g.setColour (outline);
        g.drawRoundedRectangle (bounds, noteCorner, selected ? 2.0f : 1.0f);

        // The note's name, where it fits.
        if (bounds.getHeight() >= 11.0f && bounds.getWidth() >= 30.0f)
        {
            g.setColour (getInkFor (fill).withAlpha (0.85f));
            g.setFont (juce::FontOptions (std::min (TextSize::small, bounds.getHeight() - 2.0f)));
            g.drawText (juce::MidiMessage::getMidiNoteName (data.pitch, true, true, 4), bounds.reduced (3.0f, 0.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    // What the transform would make of the selected notes.
    for (const auto& note : preview)
    {
        const auto bounds = roll.getNoteBounds (note);

        if (bounds.isEmpty() || ! bounds.intersects (area))
            continue;

        g.setColour (p.accent.withAlpha (0.45f));

        if (roll.drumMode)
            g.fillPath (makeDiamond (bounds));
        else
            g.fillRoundedRectangle (bounds, noteCorner);

        g.setColour (p.accent);

        if (roll.drumMode)
            g.strokePath (makeDiamond (bounds), juce::PathStrokeType (1.5f));
        else
            g.drawRoundedRectangle (bounds, noteCorner, 1.5f);
    }
}

//==============================================================================
void PianoRoll::NoteGrid::updateHint (juce::Point<float> position)
{
    if (roll.getTrack() == nullptr)
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        setDescription (noClipsHint);
        return;
    }

    const auto currentTool = roll.getTool();

    if (currentTool == Tool::smart || currentTool == Tool::draw)
    {
        if (const auto note = getNoteAt (position))
        {
            const auto zone = getZone (getNoteData (*note->clip, *note->note), position);
            setMouseCursor (zone == Zone::body ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::LeftRightResizeCursor);
            setDescription (zone == Zone::body ? bodyHint : (zone == Zone::end ? endHint : startHint));
            return;
        }

        if (getGhostClipAt (position) != nullptr)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            setDescription ("A ghost note, from another track's clip: double-right-click it to edit that clip. A click adds a note here.");
            return;
        }
    }

    setMouseCursor (currentTool == Tool::smart || currentTool == Tool::draw ? juce::MouseCursor::NormalCursor
                                                                            : juce::MouseCursor::CrosshairCursor);
    setDescription (describeNoteTool (currentTool));
}

void PianoRoll::NoteGrid::mouseMove (const juce::MouseEvent& e)
{
    updateHint (e.position);
}

void PianoRoll::NoteGrid::mouseDown (const juce::MouseEvent& e)
{
    gesture = nullptr;
    rubberBand = {};
    auto* ed = roll.getEdit();

    if (ed == nullptr || roll.getTrack() == nullptr)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startBeat = roll.getView().xToBeat (e.position.x);
    gesture->startPosition = e.getPosition();
    gesture->startScrollY = roll.scrollY;
    beginDragAutoRepeat (TimelineEditor::autoRepeatInterval); // so the grid goes on scrolling while the mouse rests past an edge

    auto& undo = ed->getUndoManager();
    const auto hit = getNoteAt (e.position);

    if (hit.has_value())
    {
        gesture->clip = hit->clip;
        gesture->noteState = hit->note->state;
        gesture->zone = getZone (getNoteData (*hit->clip, *hit->note), e.position);
        roll.targetClip = hit->clip;

        // New notes get the length and velocity of the note clicked last, as in FL Studio.
        roll.lastLength = hit->note->getLengthBeats();
        roll.lastVelocity = hit->note->getVelocity();
    }

    // Right-click deletes, as in FL Studio: this note and those the mouse moves on to. Shift+right-click opens the menu.
    if (e.mods.isPopupMenu())
    {
        if (e.mods.isShiftDown())
        {
            gesture = nullptr;
            showMenu();
            return;
        }

        undo.beginNewTransaction ("Delete Notes");
        gesture->kind = Gesture::Kind::erase;
        eraseOrMuteAt (e.position, true);
        return;
    }

    switch (roll.getTool())
    {
        case Tool::smart:
        case Tool::draw:
            if (hit.has_value())
            {
                // Ctrl+click adds the note to the selection or takes it out, when the mouse goes up. A click on a
                // selected note waits too: a drag moves all the selected notes.
                if (e.mods.isCommandDown() || roll.isSelected (*hit->note))
                    gesture->clickOnMouseUp = true;
                else
                    roll.selectNotes ({ hit->note }, false);

                roll.audition (*hit->clip, hit->note->getNoteNumber(), hit->note->getVelocity());
                return;
            }

            if (e.mods.isCommandDown())
            {
                gesture->kind = Gesture::Kind::select; // FL Studio's Ctrl+drag

                for (const auto& n : roll.getSelectedNotesHere())
                    gesture->selectedBefore.push_back (n.note->state);

                return;
            }

            addNoteAt (e);
            return;

        case Tool::paint:
            if (hit.has_value())
            {
                roll.selectNotes ({ hit->note }, false);
                gesture = nullptr;
                return;
            }

            undo.beginNewTransaction ("Paint Notes");
            gesture->kind = Gesture::Kind::paint;
            gesture->paintPitch = roll.getPitchForRow (roll.getRowAt (e.y));
            gesture->nextPaintStart = roll.snapDown (gesture->startBeat, e.mods);
            paintAt (e);
            return;

        case Tool::slice:
            if (hit.has_value())
            {
                const auto data = getNoteData (*hit->clip, *hit->note);
                const auto at = roll.snap (gesture->startBeat, e.mods);

                if (at > data.start + getMinimumNoteLength() && at < data.getEnd() - getMinimumNoteLength())
                {
                    auto first = data, second = data;
                    first.length = at - data.start;
                    second.start = at;
                    second.length = data.getEnd() - at;
                    undo.beginNewTransaction ("Split Note");
                    replaceNotes ({ *hit }, { first, second });
                }
            }

            gesture = nullptr;
            return;

        case Tool::mute:
        case Tool::erase:
        {
            const auto erase = roll.getTool() == Tool::erase;
            undo.beginNewTransaction (erase ? "Delete Notes" : "Mute Notes");
            gesture->kind = erase ? Gesture::Kind::erase : Gesture::Kind::mute;
            eraseOrMuteAt (e.position, erase);
            return;
        }

        case Tool::select:
            gesture->clickOnMouseUp = hit.has_value();
            return;

        case Tool::zoom:
            return; // a click or a drag, decided later

        case Tool::slip:
        default:
            gesture = nullptr;
            return;
    }
}

void PianoRoll::NoteGrid::addNoteAt (const juce::MouseEvent& e)
{
    auto* ed = roll.getEdit();
    auto pitch = roll.getPitchForRow (roll.getRowAt (e.y));

    if (ed == nullptr || pitch < 0)
    {
        gesture = nullptr;
        return;
    }

    if (const auto key = roll.getKey(); roll.snapToKey && key.has_value() && ! roll.drumMode)
        pitch = phazite::snapToKey (pitch, *key);

    // From the grid line at or before the click, in the clip it is in. On a track without clips, the note
    // starts one.
    const auto start = roll.snapDown (roll.getView().xToBeat (e.position.x), e.mods);
    auto* clip = roll.getClipForNewNote (start);

    if (clip == nullptr && ! roll.getClips().isEmpty())
    {
        gesture = nullptr;
        return;
    }

    const auto length = roll.drumMode ? te::BeatDuration::fromBeats (roll.getGridStepInBeats()) : roll.lastLength;
    auto pitches = std::vector<int> { pitch };

    if (roll.chordStamp.has_value() && ! roll.drumMode)
        pitches = getChordPitches (pitch, *roll.chordStamp);

    ed->getUndoManager().beginNewTransaction (pitches.size() > 1 ? "Stamp Chord" : "Add Note");

    if (clip == nullptr)
        clip = roll.makeClipForNewNotes();

    if (clip == nullptr)
    {
        gesture = nullptr;
        return;
    }

    juce::Array<te::MidiNote*> added;

    for (const auto p : pitches)
        if (auto* note = addNote (*clip, p, start, length, roll.lastVelocity))
            added.add (note);

    if (added.isEmpty())
    {
        gesture = nullptr;
        return;
    }

    roll.targetClip = clip;
    roll.selectNotes (added, false);
    roll.audition (*clip, pitches, roll.lastVelocity);

    // A drag that follows moves the new notes, in the same undo step.
    gesture->clip = clip;
    gesture->noteState = added.getFirst()->state;
    gesture->zone = Zone::body;
    gesture->added = true;
}

void PianoRoll::NoteGrid::eraseOrMuteAt (juce::Point<float> position, bool erase)
{
    const auto hit = getNoteAt (position);

    if (! hit.has_value() || gesture == nullptr)
        return;

    // Each note once per drag, however long the mouse stays on it.
    if (std::find (gesture->done.begin(), gesture->done.end(), hit->note->state) != gesture->done.end())
        return;

    gesture->done.push_back (hit->note->state);

    if (erase)
        deleteNotes ({ *hit });
    else
        toggleNotesMuted ({ *hit });
}

void PianoRoll::NoteGrid::paintAt (const juce::MouseEvent& e)
{
    if (gesture == nullptr || gesture->paintPitch < 0)
        return;

    // A note at each step of the grid the mouse has passed, on the row the drag began on.
    const auto step = te::BeatDuration::fromBeats (roll.getGridStepInBeats());
    const auto beat = roll.getView().xToBeat (e.position.x);

    while (gesture->nextPaintStart <= beat)
    {
        const auto start = gesture->nextPaintStart;
        gesture->nextPaintStart = start + step;

        const auto notes = roll.getAllNotes();
        const auto taken = std::any_of (notes.begin(), notes.end(),
                                        [this, start] (const ClipNote& n)
                                        {
                                            return n.note->getNoteNumber() == gesture->paintPitch
                                                   && std::abs ((getNoteStart (*n.clip, *n.note) - start).inBeats()) < 1.0e-6;
                                        });

        if (taken)
            continue;

        // On a track without clips, the first note starts one.
        auto* clip = roll.getClipForNewNote (start);

        if (clip == nullptr && roll.getClips().isEmpty())
            clip = roll.makeClipForNewNotes();

        if (clip != nullptr)
            if (auto* note = addNote (*clip, gesture->paintPitch, start, step, roll.lastVelocity))
                roll.selectNotes ({ note }, true);
    }
}

//==============================================================================
void PianoRoll::NoteGrid::beginDrag (const juce::MouseEvent& e)
{
    auto* ed = roll.getEdit();
    gesture->clickOnMouseUp = false;

    if (roll.getTool() == Tool::zoom)
    {
        gesture->kind = Gesture::Kind::zoom;
        return;
    }

    if (roll.getTool() == Tool::select)
    {
        gesture->kind = Gesture::Kind::select;

        if (e.mods.isCommandDown())
            for (const auto& n : roll.getSelectedNotesHere())
                gesture->selectedBefore.push_back (n.note->state);

        return;
    }

    auto* clip = gesture->clip.get();

    if (ed == nullptr || clip == nullptr)
        return; // nothing under the mouse to drag

    auto notes = roll.getSelectedNotesHere();
    const auto copy = gesture->zone == Zone::body && e.mods.isShiftDown();

    if (! gesture->added)
        ed->getUndoManager().beginNewTransaction (gesture->zone != Zone::body ? "Resize Notes" : (copy ? "Copy Notes" : "Move Notes"));

    // Shift+drag copies the notes first, as in FL Studio, and the copies move.
    if (copy)
    {
        const auto anchorIndex =
            std::find_if (notes.begin(), notes.end(), [this] (const ClipNote& n) { return n.note->state == gesture->noteState; })
            - notes.begin();
        const auto copies = replaceNotes ({}, getNoteData (notes));
        juce::Array<te::MidiNote*> copied;

        for (const auto& n : copies)
            copied.add (n.note);

        roll.selectNotes (copied, false);

        if (juce::isPositiveAndBelow ((int) anchorIndex, (int) copies.size()))
            gesture->noteState = copies[(size_t) anchorIndex].note->state;

        notes = copies;
    }

    for (const auto& n : notes)
    {
        const Gesture::Original original { n.clip, n.note->state, getNoteStart (*n.clip, *n.note), n.note->getLengthBeats(),
                                           n.note->getNoteNumber() };
        gesture->originals.push_back (original);

        if (n.note->state == gesture->noteState)
            gesture->anchor = original;
    }

    if (gesture->anchor.state.isValid())
        gesture->kind = gesture->zone == Zone::body ? Gesture::Kind::move
                                                    : (gesture->zone == Zone::end ? Gesture::Kind::resizeEnd : Gesture::Kind::resizeStart);
}

void PianoRoll::NoteGrid::dragNotes (const juce::MouseEvent& e)
{
    const auto beat = roll.getView().xToBeat (e.position.x);
    const auto& anchor = gesture->anchor;
    const auto findNote = [] (const Gesture::Original& original) -> te::MidiNote*
    {
        auto* clip = original.clip.get();
        return clip != nullptr ? clip->getSequence().getNoteFor (original.state) : nullptr;
    };

    if (gesture->kind == Gesture::Kind::move)
    {
        // The note under the mouse snaps to the grid, and the others keep their distance from it; none goes
        // before its clip's start.
        auto delta = roll.snap (anchor.start + (beat - gesture->startBeat), e.mods) - anchor.start;

        for (const auto& original : gesture->originals)
            if (auto* clip = original.clip.get())
                delta = std::max (delta, clip->getStartBeat() - original.start);

        // Up or down by rows, as far as every note stays on one. Past an edge, the row there counts (the rows
        // scroll along).
        const auto rowAtPointer = roll.getRowAt (juce::jlimit (0, getHeight() - 1, e.y));
        const auto mouseRow = rowAtPointer >= 0 ? rowAtPointer : roll.getNumRows() - 1;
        auto rowDelta = mouseRow - roll.getRowForPitch (anchor.pitch);

        for (const auto& original : gesture->originals)
            if (const auto row = roll.getRowForPitch (original.pitch); row >= 0)
                rowDelta = juce::jlimit (-row, roll.getNumRows() - 1 - row, rowDelta);

        for (const auto& original : gesture->originals)
        {
            auto* note = findNote (original);
            const auto row = roll.getRowForPitch (original.pitch);

            if (note == nullptr || row < 0)
                continue;

            auto pitch = roll.getPitchForRow (row + rowDelta);

            if (const auto key = roll.getKey(); roll.snapToKey && key.has_value() && ! roll.drumMode)
                pitch = phazite::snapToKey (pitch, *key);

            moveNote (*original.clip.get(), *note, original.start + delta, pitch);
        }

        // The note under the mouse plays when it changes pitch.
        if (auto* note = findNote (anchor); note != nullptr && note->getNoteNumber() != gesture->auditionedPitch)
        {
            gesture->auditionedPitch = note->getNoteNumber();
            roll.audition (*anchor.clip.get(), note->getNoteNumber(), note->getVelocity());
        }

        return;
    }

    const auto minimum = getMinimumNoteLength();

    if (gesture->kind == Gesture::Kind::resizeEnd)
    {
        const auto end = roll.snap (anchor.start + anchor.length + (beat - gesture->startBeat), e.mods);
        const auto change = end - (anchor.start + anchor.length);

        for (const auto& original : gesture->originals)
            if (auto* note = findNote (original))
                setNoteLength (*original.clip.get(), *note, original.length + change);

        if (auto* note = findNote (anchor))
            roll.lastLength = note->getLengthBeats();

        return;
    }

    if (gesture->kind == Gesture::Kind::resizeStart)
    {
        // The starts move and the ends stay, but no note gets shorter than the minimum or starts before its clip.
        auto change = roll.snap (anchor.start + (beat - gesture->startBeat), e.mods) - anchor.start;

        for (const auto& original : gesture->originals)
        {
            change = std::min (change, original.length - minimum);

            if (auto* clip = original.clip.get())
                change = std::max (change, clip->getStartBeat() - original.start);
        }

        for (const auto& original : gesture->originals)
        {
            if (auto* note = findNote (original))
            {
                moveNote (*original.clip.get(), *note, original.start + change, original.pitch);
                setNoteLength (*original.clip.get(), *note, original.length - change);
            }
        }
    }
}

void PianoRoll::NoteGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (gesture == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        if (e.getDistanceFromDragStart() < dragThreshold)
            return;

        beginDrag (e);
    }

    // Past an edge, the grid scrolls along, and the drag goes on from what is under the pointer then.
    if (const auto directions = gesture->getAutoScrollDirections())
        roll.autoScroll (e.getPosition(), getLocalBounds(), *directions);

    switch (gesture->kind)
    {
        case Gesture::Kind::move:
        case Gesture::Kind::resizeStart:
        case Gesture::Kind::resizeEnd:
            dragNotes (e);
            break;

        case Gesture::Kind::select:
        {
            rubberBand = juce::Rectangle<int> (getGestureStart(), e.getPosition());
            juce::Array<te::MidiNote*> inside;

            for (const auto& n : roll.getAllNotes())
                if (roll.getNoteBounds (getNoteData (*n.clip, *n.note)).intersects (rubberBand.toFloat())
                    || std::find (gesture->selectedBefore.begin(), gesture->selectedBefore.end(), n.note->state)
                           != gesture->selectedBefore.end())
                    inside.add (n.note);

            roll.selectNotes (inside, false);
            repaint();
            break;
        }

        case Gesture::Kind::paint:
            paintAt (e);
            break;

        case Gesture::Kind::erase:
        case Gesture::Kind::mute:
            eraseOrMuteAt (e.position, gesture->kind == Gesture::Kind::erase);
            break;

        case Gesture::Kind::zoom:
            rubberBand = juce::Rectangle<int> (getGestureStart(), e.getPosition()).withY (0).withHeight (getHeight());
            repaint();
            break;

        case Gesture::Kind::pending:
        default:
            break;
    }
}

void PianoRoll::NoteGrid::mouseUp (const juce::MouseEvent& e)
{
    roll.stopAudition();

    if (gesture == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        auto* clip = gesture->clip.get();
        auto* note = clip != nullptr ? clip->getSequence().getNoteFor (gesture->noteState) : nullptr;

        if (roll.getTool() == Tool::zoom)
        {
            roll.zoomAround (e.position.x, e.mods.isAltDown() ? 0.5 : 2.0);
        }
        else if (note != nullptr && gesture->clickOnMouseUp)
        {
            // A Ctrl+click toggles the note; a click on a selected note selects it alone.
            if (e.mods.isCommandDown())
            {
                juce::Array<te::MidiNote*> notes;

                for (const auto& n : roll.getSelectedNotesHere())
                    if (n.note != note)
                        notes.add (n.note);

                if (! roll.isSelected (*note))
                    notes.add (note);

                roll.selectNotes (notes, false);
            }
            else
            {
                roll.selectNotes ({ note }, false);
            }
        }
        else if (roll.getTool() == Tool::select && note == nullptr && ! e.mods.isCommandDown())
        {
            roll.selectNotes ({}, false);
        }
    }
    else if (gesture->kind == Gesture::Kind::zoom && rubberBand.getWidth() > dragThreshold)
    {
        roll.zoomTo ({ roll.getView().xToBeat (rubberBand.getX()), roll.getView().xToBeat (rubberBand.getRight()) });
    }

    gesture = nullptr;

    if (! rubberBand.isEmpty())
    {
        rubberBand = {};
        repaint();
    }

    updateHint (e.position);
}

juce::Point<int> PianoRoll::NoteGrid::getGestureStart() const
{
    // The grid may have scrolled since.
    return { juce::roundToInt (roll.getView().beatToX (gesture->startBeat)),
             gesture->startPosition.y + gesture->startScrollY - roll.scrollY };
}

void PianoRoll::NoteGrid::cancelGesture()
{
    // What the drag did so far stays, as one undo step.
    roll.stopAudition();
    gesture = nullptr;
    rubberBand = {};
    updateHint (getMouseXYRelative().toFloat());
    repaint();
}

void PianoRoll::NoteGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    // A double right-click on a ghost note edits its clip, as in FL Studio.
    if (e.mods.isPopupMenu())
        if (auto* clip = getGhostClipAt (e.position))
            roll.selection.selectOnly (clip);
}

void PianoRoll::NoteGrid::showMenu()
{
    auto& registry = roll.getCommands();
    juce::PopupMenu menu;
    addCommandItems (menu, registry, { "edit.duplicate", "edit.delete", "edit.mute" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "edit.quantize", "note.legato", "note.chop" });
    addCommandItems (menu, registry, { "note.quantizeSettings", "note.legatoSettings", "note.chopSettings", "note.transposeSettings" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "note.selectAll", "edit.deselectAll" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "pianoRoll.drumMode", "pianoRoll.ghostNotes", "pianoRoll.snapToKey" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}
} // namespace phazite::ui
