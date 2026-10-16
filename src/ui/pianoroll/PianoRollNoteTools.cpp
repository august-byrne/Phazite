// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's tools: what the mouse does in the note grid, from adding and painting notes to moving, resizing,
// deleting and muting them, with the selection rectangle and the grid's menu.
#include "PianoRollParts.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"
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
    auto* ed = roll.getEdit();

    if (ed == nullptr || roll.getTrack() == nullptr)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startBeat = roll.getView().xToBeat (e.position.x);
    gesture->startPosition = e.getPosition();
    gesture->startScrollY = roll.scrollY;
    beginDragAutoRepeat (TimelineEditor::autoRepeatInterval); // so the grid goes on scrolling while the mouse rests past an edge

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

            gesture->beginStep (roll.context, "Paint Notes");
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
                    gesture->beginStep (roll.context, "Split Note");
                    replaceNotes (*roll.getEdit(), { *hit }, { first, second });
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
    const auto start = toTracktion (roll.snapDown (roll.getView().xToBeat (e.position.x), e.mods));
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

    gesture->beginStep (roll.context, pitches.size() > 1 ? "Stamp Chord" : "Add Note");

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
}

void PianoRoll::NoteGrid::eraseOrMuteAt (juce::Point<float> position, bool erase)
{
    const auto hit = getNoteAt (position);

    if (! hit.has_value() || gesture == nullptr)
        return;

    // Each note once per drag, however long the mouse stays on it.
    if (! gesture->doOnce (hit->note->state))
        return;

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
    const auto step = BeatDuration::fromBeats (roll.getGridStepInBeats());
    const auto beat = roll.getView().xToBeat (e.position.x);

    while (gesture->nextPaintStart <= beat)
    {
        const auto start = toTracktion (gesture->nextPaintStart);
        gesture->nextPaintStart = gesture->nextPaintStart + step;

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
            if (auto* note = addNote (*clip, gesture->paintPitch, start, toTracktion (step), roll.lastVelocity))
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

    // A note added by this click and the drag that moves it are one step, the click's.
    gesture->beginStep (roll.context, gesture->zone != Zone::body ? "Resize Notes" : (copy ? "Copy Notes" : "Move Notes"));

    // Shift+drag copies the notes first, as in FL Studio, and the copies move.
    if (copy)
    {
        const auto anchorIndex =
            std::find_if (notes.begin(), notes.end(), [this] (const ClipNote& n) { return n.note->state == gesture->noteState; })
            - notes.begin();
        const auto copies = replaceNotes (*ed, {}, getNoteData (notes));
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
    // In Tracktion's beats, as the notes are, until the piano roll works by IDs (architecture phase A3).
    const auto beat = toTracktion (roll.getView().xToBeat (e.position.x));
    const auto startBeat = toTracktion (gesture->startBeat);
    const auto snapped = [this, &e] (te::BeatPosition b) { return toTracktion (roll.snap (fromTracktion (b), e.mods)); };
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
        auto delta = snapped (anchor.start + (beat - startBeat)) - anchor.start;

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

    const auto minimum = toTracktion (getMinimumNoteLength());

    if (gesture->kind == Gesture::Kind::resizeEnd)
    {
        const auto end = snapped (anchor.start + anchor.length + (beat - startBeat));
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
        auto change = snapped (anchor.start + (beat - startBeat)) - anchor.start;

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
