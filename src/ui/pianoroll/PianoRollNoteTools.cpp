// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's tools: what the mouse does in the note grid, from adding and painting notes to moving, resizing,
// deleting and muting them, with the selection rectangle and the grid's menu.
#include "PianoRollParts.h"
#include "ui/input/CommandUI.h"

#include <algorithm>

namespace phazite::ui
{
PianoRoll::NoteGrid::~NoteGrid() = default;

void PianoRoll::NoteGrid::mouseMove (const juce::MouseEvent& e)
{
    updateHint (e.position);
}

void PianoRoll::NoteGrid::mouseDown (const juce::MouseEvent& e)
{
    gesture = nullptr;
    rubberBand = {};

    if (! roll.hasProject() || ! roll.getTrack().isValid())
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startBeat = roll.getView().xToBeat (e.position.x);
    gesture->startPosition = e.getPosition();
    gesture->startScrollY = roll.scrollY;
    beginDragAutoRepeat (TimelineEditor::autoRepeatInterval); // so the grid goes on scrolling while the mouse rests past an edge

    const auto hit = getNoteAt (e.position);

    if (hit.has_value())
    {
        gesture->note = hit->getRef();
        gesture->zone = getZone (*hit, e.position);
        roll.targetClip = hit->clip;

        // New notes get the length and velocity of the note clicked last, as in FL Studio.
        roll.lastLength = hit->length;
        roll.lastVelocity = hit->velocity;
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

        gesture->beginStep (roll.context, "Delete Notes");
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
                if (e.mods.isCommandDown() || roll.getSelectedNoteRefs().contains (hit->getRef()))
                    gesture->clickOnMouseUp = true;
                else
                    roll.selectNotes ({ hit->getRef() }, false);

                roll.audition (hit->clip, hit->pitch, hit->velocity);
                return;
            }

            if (e.mods.isCommandDown())
            {
                gesture->kind = Gesture::Kind::select; // FL Studio's Ctrl+drag
                gesture->selectedBefore = roll.getSelectedNoteRefs();
                return;
            }

            addNoteAt (e);
            return;

        case Tool::paint:
            if (hit.has_value())
            {
                roll.selectNotes ({ hit->getRef() }, false);
                gesture = nullptr;
                return;
            }

            gesture->beginStep (roll.context, "Paint Notes");
            gesture->kind = Gesture::Kind::paint;
            gesture->paintPitch = roll.getPitchForRow (roll.getRowAt (e.y));
            gesture->nextPaintStart = roll.snapDown (gesture->startBeat, e.mods);
            paintAt (e);
            return;

        case Tool::slice:
            if (hit.has_value())
            {
                const auto at = roll.snap (gesture->startBeat, e.mods);

                if (at > hit->start + getMinimumNoteLength() && at < hit->getEnd() - getMinimumNoteLength())
                {
                    // The left part stays the note it was (replaceNotes() keeps its ID), and the right one is new.
                    auto first = *hit, second = *hit;
                    first.length = at - hit->start;
                    second.start = at;
                    second.length = hit->getEnd() - at;
                    gesture->beginStep (roll.context, "Split Note");
                    replaceNotes (roll.context, { hit->getRef() }, { first, second });
                }
            }

            gesture = nullptr;
            return;

        case Tool::mute:
        case Tool::erase:
        {
            const auto erase = roll.getTool() == Tool::erase;
            gesture->beginStep (roll.context, erase ? "Delete Notes" : "Mute Notes");
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
    auto pitch = roll.getPitchForRow (roll.getRowAt (e.y));

    if (! roll.hasProject() || pitch < 0)
    {
        gesture = nullptr;
        return;
    }

    if (const auto key = roll.getKey(); roll.snapToKey && key.has_value() && ! roll.drumMode)
        pitch = phazite::snapToKey (pitch, *key);

    // From the grid line at or before the click, in the clip it is in. On a track without clips, the note
    // starts one.
    const auto start = roll.snapDown (roll.getView().xToBeat (e.position.x), e.mods);
    auto clip = roll.getClipForNewNote (start);

    if (! clip.isValid() && ! roll.getShownClips().empty())
    {
        gesture = nullptr;
        return;
    }

    const auto length = roll.drumMode ? BeatDuration::fromBeats (roll.getGridStepInBeats()) : roll.lastLength;
    auto pitches = std::vector<int> { pitch };

    if (roll.chordStamp.has_value() && ! roll.drumMode)
        pitches = getChordPitches (pitch, *roll.chordStamp);

    gesture->beginStep (roll.context, pitches.size() > 1 ? "Stamp Chord" : "Add Note");

    if (! clip.isValid())
        clip = roll.makeClipForNewNotes();

    if (! clip.isValid())
    {
        gesture = nullptr;
        return;
    }

    std::vector<NoteRef> added;

    for (const auto p : pitches)
        if (const auto note = addNote (roll.context, clip, p, start, length, roll.lastVelocity); note.isValid())
            added.push_back (note);

    if (added.empty())
    {
        gesture = nullptr;
        return;
    }

    roll.targetClip = clip;
    roll.selectNotes (added, false);
    roll.audition (clip, pitches, roll.lastVelocity);

    // A drag that follows moves the new notes, in the same undo step.
    gesture->note = added.front();
    gesture->zone = Zone::body;
}

void PianoRoll::NoteGrid::eraseOrMuteAt (juce::Point<float> position, bool erase)
{
    const auto hit = getNoteAt (position);

    if (! hit.has_value() || gesture == nullptr)
        return;

    // Each note once per drag, however long the mouse stays on it.
    if (! gesture->doOnce (hit->getRef()))
        return;

    if (erase)
        deleteNotes (roll.context, { hit->getRef() });
    else
        toggleNotesMuted (roll.context, { hit->getRef() });
}

void PianoRoll::NoteGrid::paintAt (const juce::MouseEvent& e)
{
    if (gesture == nullptr || gesture->paintPitch < 0)
        return;

    // A note at each step of the grid the mouse has passed, on the row the drag began on.
    const auto step = BeatDuration::fromBeats (roll.getGridStepInBeats());
    const auto beat = roll.getView().xToBeat (e.position.x);

    while (gesture->nextPaintStart <= beat)
    {
        const auto start = gesture->nextPaintStart;
        gesture->nextPaintStart = gesture->nextPaintStart + step;

        const auto notes = roll.getAllNotes();
        const auto taken =
            std::any_of (notes.begin(), notes.end(), [this, start] (const NoteData& note)
                         { return note.pitch == gesture->paintPitch && std::abs ((note.start - start).inBeats()) < 1.0e-6; });

        if (taken)
            continue;

        // On a track without clips, the first note starts one.
        auto clip = roll.getClipForNewNote (start);

        if (! clip.isValid() && roll.getShownClips().empty())
            clip = roll.makeClipForNewNotes();

        if (clip.isValid())
            if (const auto note = addNote (roll.context, clip, gesture->paintPitch, start, step, roll.lastVelocity); note.isValid())
                roll.selectNotes ({ note }, true);
    }
}

//==============================================================================
void PianoRoll::NoteGrid::beginDrag (const juce::MouseEvent& e)
{
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
            gesture->selectedBefore = roll.getSelectedNoteRefs();

        return;
    }

    if (! roll.hasProject() || ! gesture->note.isValid())
        return; // nothing under the mouse to drag

    auto notes = roll.getSelectedNotesHere();
    const auto copy = gesture->zone == Zone::body && e.mods.isShiftDown();

    // A note added by this click and the drag that moves it are one step, the click's.
    gesture->beginStep (roll.context, gesture->zone != Zone::body ? "Resize Notes" : (copy ? "Copy Notes" : "Move Notes"));

    // Shift+drag copies the notes first, as in FL Studio, and the copies move.
    if (copy)
    {
        const auto anchorIndex =
            std::find_if (notes.begin(), notes.end(), [this] (const NoteData& n) { return n.getRef() == gesture->note; }) - notes.begin();
        const auto copies = replaceNotes (roll.context, {}, notes);
        roll.selectNotes (copies, false);

        if (juce::isPositiveAndBelow ((int) anchorIndex, (int) copies.size()))
            gesture->note = copies[(size_t) anchorIndex];

        notes.clear();

        for (const auto& ref : copies)
            if (const auto note = getNote (roll.context, ref))
                notes.push_back (*note);
    }

    // Where each note's clip starts: no note goes before it.
    const auto shownClips = roll.getShownClips();

    for (const auto& n : notes)
    {
        const auto clip = std::find_if (shownClips.begin(), shownClips.end(), [&n] (const NoteClip& c) { return c.id == n.clip; });
        const Gesture::Original original { n.getRef(), n.start, n.length, n.pitch,
                                           clip != shownClips.end() ? clip->range.getStart() : BeatPosition() };
        gesture->originals.push_back (original);

        if (n.getRef() == gesture->note)
            gesture->anchor = original;
    }

    if (gesture->anchor.note.isValid())
        gesture->kind = gesture->zone == Zone::body ? Gesture::Kind::move
                                                    : (gesture->zone == Zone::end ? Gesture::Kind::resizeEnd : Gesture::Kind::resizeStart);
}

void PianoRoll::NoteGrid::dragNotes (const juce::MouseEvent& e)
{
    const auto beat = roll.getView().xToBeat (e.position.x);
    const auto snapped = [this, &e] (BeatPosition b) { return roll.snap (b, e.mods); };
    const auto& anchor = gesture->anchor;
    std::vector<NoteData> moved; // where the drag puts each note

    const auto add = [&moved] (const Gesture::Original& original, BeatPosition start, BeatDuration length, int pitch)
    { moved.push_back ({ .clip = original.note.clip, .id = original.note.note, .pitch = pitch, .start = start, .length = length }); };

    if (gesture->kind == Gesture::Kind::move)
    {
        // The note under the mouse snaps to the grid, and the others keep their distance from it; none goes
        // before its clip's start.
        auto delta = snapped (anchor.start + (beat - gesture->startBeat)) - anchor.start;

        for (const auto& original : gesture->originals)
            delta = std::max (delta, original.clipStart - original.start);

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
            const auto row = roll.getRowForPitch (original.pitch);

            if (row < 0)
                continue;

            auto pitch = roll.getPitchForRow (row + rowDelta);

            if (const auto key = roll.getKey(); roll.snapToKey && key.has_value() && ! roll.drumMode)
                pitch = phazite::snapToKey (pitch, *key);

            add (original, original.start + delta, original.length, pitch);
        }

        setNotes (roll.context, moved);

        // The note under the mouse plays when it changes pitch.
        if (const auto note = getNote (roll.context, anchor.note); note.has_value() && note->pitch != gesture->auditionedPitch)
        {
            gesture->auditionedPitch = note->pitch;
            roll.audition (anchor.note.clip, note->pitch, note->velocity);
        }

        return;
    }

    if (gesture->kind == Gesture::Kind::resizeEnd)
    {
        const auto end = snapped (anchor.start + anchor.length + (beat - gesture->startBeat));
        const auto change = end - (anchor.start + anchor.length);

        for (const auto& original : gesture->originals)
            add (original, original.start, original.length + change, original.pitch);

        setNotes (roll.context, moved);

        if (const auto note = getNote (roll.context, anchor.note))
            roll.lastLength = note->length;

        return;
    }

    if (gesture->kind == Gesture::Kind::resizeStart)
    {
        // The starts move and the ends stay, but no note gets shorter than the minimum or starts before its clip.
        auto change = snapped (anchor.start + (beat - gesture->startBeat)) - anchor.start;

        for (const auto& original : gesture->originals)
        {
            change = std::min (change, original.length - getMinimumNoteLength());
            change = std::max (change, original.clipStart - original.start);
        }

        for (const auto& original : gesture->originals)
            add (original, original.start + change, original.length - change, original.pitch);

        setNotes (roll.context, moved);
    }
}

void PianoRoll::NoteGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (gesture == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        if (! TimelineGesture::isPastDragThreshold (e))
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
            std::vector<NoteRef> inside;

            for (const auto& note : roll.getAllNotes())
                if (roll.getNoteBounds (note).intersects (rubberBand.toFloat()) || gesture->selectedBefore.contains (note.getRef()))
                    inside.push_back (note.getRef());

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
        const auto note = gesture->note;
        const auto noteIsThere = note.isValid() && getNote (roll.context, note).has_value();

        if (roll.getTool() == Tool::zoom)
        {
            roll.zoomAround (e.position.x, e.mods.isAltDown() ? 0.5 : 2.0);
        }
        else if (noteIsThere && gesture->clickOnMouseUp)
        {
            // A Ctrl+click toggles the note; a click on a selected note selects it alone.
            if (e.mods.isCommandDown())
            {
                auto notes = roll.getSelectedRefsHere();
                const auto wasSelected = std::find (notes.begin(), notes.end(), note) != notes.end();
                notes.erase (std::remove (notes.begin(), notes.end(), note), notes.end());

                if (! wasSelected)
                    notes.push_back (note);

                roll.selectNotes (notes, false);
            }
            else
            {
                roll.selectNotes ({ note }, false);
            }
        }
        else if (roll.getTool() == Tool::select && ! note.isValid() && ! e.mods.isCommandDown())
        {
            roll.selectNotes ({}, false);
        }
    }
    else if (gesture->kind == Gesture::Kind::zoom && rubberBand.getWidth() > TimelineGesture::dragThreshold)
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
    return gesture->getStart (roll.getView(), roll.scrollY); // the grid may have scrolled since
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
        if (const auto clip = getGhostClipAt (e.position); clip.isValid())
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
