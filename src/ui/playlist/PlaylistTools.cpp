// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tools: what the mouse does in the lanes (ui/timeline/SmartTool.h), and the edits of the selected clips
// that the time line's commands make.
#include "PlaylistParts.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LiveInput.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    const juce::String emptySpaceHint ("Click to select the track, drag to select a time range, and double-click on a track with an "
                                       "instrument to add a MIDI clip, or under the last track to add a track. Ctrl+wheel zooms; "
                                       "Alt+wheel resizes the tracks.");

    juce::MouseCursor::StandardCursorType getCursorFor (ClipZone zone)
    {
        switch (zone)
        {
            case ClipZone::trimStart:
            case ClipZone::trimEnd:
                return juce::MouseCursor::LeftRightResizeCursor;
            case ClipZone::fadeIn:
            case ClipZone::fadeOut:
                return juce::MouseCursor::CrosshairCursor;
            case ClipZone::selectRange:
                return juce::MouseCursor::IBeamCursor;
            case ClipZone::move:
            default:
                return juce::MouseCursor::DraggingHandCursor;
        }
    }

    juce::MouseCursor::StandardCursorType getCursorFor (Tool tool)
    {
        switch (tool)
        {
            case Tool::draw:
            case Tool::paint:
            case Tool::select:
            case Tool::zoom:
                return juce::MouseCursor::CrosshairCursor;
            case Tool::slice:
                return juce::MouseCursor::IBeamCursor;
            case Tool::slip:
                return juce::MouseCursor::LeftRightResizeCursor;
            case Tool::mute:
            case Tool::erase:
                return juce::MouseCursor::PointingHandCursor;
            case Tool::smart:
            default:
                return juce::MouseCursor::NormalCursor;
        }
    }
} // namespace

//==============================================================================
void Playlist::clipClicked (te::Clip& clip, const juce::ModifierKeys& mods)
{
    range.clear();

    // Ctrl or Shift adds the clip to the selection, or takes it out.
    if (! (mods.isCommandDown() || mods.isShiftDown()))
        selection.selectOnly (clip);
    else if (selection.isSelected (clip))
        selection.deselect (&clip);
    else
        selection.addToSelection (clip);
}

void Playlist::showClipMenu()
{
    auto& bridge = getCommands();
    juce::PopupMenu menu;
    addCommandItems (menu, bridge, { "edit.split", "edit.duplicate", "edit.duplicateAsAlias", "clip.makeUnique" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "edit.mute", "edit.quantize" });

    juce::PopupMenu fadeShapes;
    addCommandItems (fadeShapes, bridge, { "clip.fadeLinear", "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve" });
    menu.addSubMenu ("Fade Shape", fadeShapes);

    menu.addSeparator();
    addCommandItems (menu, bridge, { "edit.delete", "edit.deleteAndCloseGap" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::selectRange (te::BeatPosition from, te::BeatPosition to, int fromRow, int toRow)
{
    juce::Array<te::Track*> tracks;

    for (auto row = std::min (fromRow, toRow); row <= std::max (fromRow, toRow); ++row)
        if (juce::isPositiveAndBelow (row, (int) rows.size()))
            if (auto* track = rows[(size_t) row].track.get())
                tracks.add (track);

    range.set (te::BeatRange (std::min (from, to), std::max (from, to)), tracks);
}

std::optional<Playlist::CrossfadeSpot> Playlist::findCrossfadeAt (juce::Point<int> position) const
{
    auto* e = getEdit();
    const auto row = getRowAt (position.y);

    if (e == nullptr || row < 0)
        return {};

    // Only the lower third of a row, so the upper part still trims.
    const auto& r = rows[(size_t) row];
    auto* track = dynamic_cast<te::ClipTrack*> (r.track.get());

    if (track == nullptr || position.y < getRowTop (r) + r.height * 2 / 3)
        return {};

    const auto xOf = [this, e] (te::TimePosition time) { return getView().beatToX (e->tempoSequence.toBeats (time)); };

    for (auto* a : track->getClips())
    {
        for (auto* b : track->getClips())
        {
            auto* first = dynamic_cast<te::AudioClipBase*> (a);
            auto* second = dynamic_cast<te::AudioClipBase*> (b);

            if (first == nullptr || second == nullptr || first == second)
                continue;

            // The second starts after the first, where it ends or inside it (a crossfade already), and ends later.
            const auto one = first->getEditTimeRange(), two = second->getEditTimeRange();

            if (two.getStart() <= one.getStart() || two.getEnd() <= one.getEnd() || xOf (two.getStart()) > xOf (one.getEnd()) + 1.0)
                continue;

            const auto boundary = te::TimePosition::fromSeconds ((one.getEnd().inSeconds() + two.getStart().inSeconds()) / 2.0);

            if (std::abs (xOf (boundary) - position.x) <= 6.0)
                return CrossfadeSpot { first, second, boundary };
        }
    }

    return {};
}

te::Clip* Playlist::placeClip (int row, te::BeatPosition beat, bool snapStart)
{
    auto* track =
        juce::isPositiveAndBelow (row, (int) rows.size()) ? dynamic_cast<te::ClipTrack*> (rows[(size_t) row].track.get()) : nullptr;

    if (track == nullptr)
        return nullptr;

    // From the grid line at or before the beat.
    return phazite::placeClip (*track, snapStart ? snapDown (beat, {}) : beat, lastClip.get());
}

void Playlist::muteOrDeleteClipAt (juce::Point<int> position, bool deleteIt)
{
    auto* component = getClipComponentAt (position);
    auto* clip = component != nullptr ? component->getClip() : nullptr;

    if (clip == nullptr || gesture == nullptr)
        return;

    // Each clip once per drag, however long the mouse stays on it.
    if (! gesture->doOnce (clip->state))
        return;

    if (deleteIt)
        deleteClips ({ clip }, false);
    else
        toggleClipsMuted ({ clip });
}

void Playlist::selectClipsIn (juce::Rectangle<int> area, bool addToSelection)
{
    juce::Array<te::Selectable*> items;

    if (addToSelection && gesture != nullptr)
        for (const auto& before : gesture->selectedBefore)
            if (auto* item = before.get())
                items.add (item);

    for (auto* component : clipComponents)
        if (auto* clip = component->getClip(); clip != nullptr && getClipBounds (*clip).intersects (area))
            items.addIfNotAlreadyThere (clip);

    range.clear();
    selection.deselectAll();

    for (auto* item : items)
        selection.addToSelection (item);
}

//==============================================================================
bool Playlist::canNudgeSelection() const
{
    return ! range.isEmpty() || ! getSelectedClips (selection).isEmpty();
}

void Playlist::nudgeSelection (int direction)
{
    // A step of the grid as it is now, so what is seen is what moves.
    const auto step = te::BeatDuration::fromBeats (direction * getGridStepInBeats());

    if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
    {
        nudgeClips (clips, step);
        return;
    }

    if (! range.isEmpty())
    {
        const auto r = range.getRange();
        const auto start = te::BeatPosition::fromBeats (std::max (0.0, (r.getStart() + step).inBeats()));
        range.set ({ start, start + r.getLength() }, range.getTracks());
    }
}

bool Playlist::canQuantizeSelection() const
{
    return ! getSelectedClips (selection).isEmpty();
}

void Playlist::quantizeSelection()
{
    auto* e = getEdit();

    if (e == nullptr)
        return;

    // To the grid as it is now, whether or not snapping is on: quantizing is asked for explicitly.
    for (auto* clip : getSelectedClips (selection))
        if (auto* track = clip->getClipTrack())
            moveClip (*clip, snapToGrid (clip->getStartBeat(), getSnapType(), e->tempoSequence, getGrid().triplets), *track);
}

//==============================================================================
void Playlist::toolChanged()
{
    gesture = nullptr;
    rubberBand = {};

    lanes->setMouseCursor (getCursorFor (getTool()));
    lanes->setDescription (getTool() == Tool::smart ? emptySpaceHint : describeTool (getTool()));
    lanes->repaint();
}

void Playlist::lanesMouseMove (const juce::MouseEvent& e)
{
    const auto currentTool = getTool();

    // The hint bar and the pointer say what the mouse does here. With the Smart Tool, and with Draw over a
    // clip, that depends on where on the clip it is.
    if (currentTool == Tool::smart && findCrossfadeAt (e.getPosition()).has_value())
    {
        lanes->setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        lanes->setDescription ("Drag to crossfade the two clips: each plays over the other while one fades out and the other in.");
        return;
    }

    auto* component = getClipComponentAt (e.getPosition());

    if (component != nullptr && (currentTool == Tool::smart || currentTool == Tool::draw))
    {
        const auto zone = getClipZone (getClipBounds (*component->getClip()).toFloat(), e.position,
                                       dynamic_cast<te::AudioClipBase*> (component->getClip()) != nullptr);
        lanes->setMouseCursor (getCursorFor (zone));
        lanes->setDescription (describeClipZone (zone));
        return;
    }

    lanes->setMouseCursor (getCursorFor (currentTool));
    lanes->setDescription (currentTool == Tool::smart ? emptySpaceHint : describeTool (currentTool));
}

void Playlist::lanesMouseDown (const juce::MouseEvent& e)
{
    gesture = nullptr;
    auto* ed = getEdit();

    if (ed == nullptr)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startPosition = e.getPosition();
    gesture->startBeat = getView().xToBeat (e.position.x);
    gesture->startRow = getRowAt (e.y);
    gesture->startScrollY = scrollY;
    beginDragAutoRepeat (autoRepeatInterval); // so the lanes go on scrolling while the mouse rests past an edge

    auto* component = getClipComponentAt (e.getPosition());
    auto* clip = component != nullptr ? component->getClip() : nullptr;
    auto currentTool = getTool();

    if (e.mods.isPopupMenu())
    {
        // In FL Studio's keymap, right-click deletes, as in FL Studio: this clip and those the mouse moves on to.
        // Shift+right-click opens the menu, as in the piano roll.
        if (getCommands().getKeymap().rightClickDeletes && ! e.mods.isShiftDown())
        {
            gesture->beginStep (*ed, "Delete Clips");
            gesture->kind = Gesture::Kind::erase;
            muteOrDeleteClipAt (e.getPosition(), true);
        }
        else
        {
            smartToolDown (e); // the menus
        }

        return;
    }

    // With FL Studio's keymap, the Smart Tool places the last clip used where empty space is clicked, as FL's Draw
    // tool does. Ctrl+drag still selects a time range there.
    const auto placesForSmartTool = currentTool == Tool::smart && clip == nullptr && getCommands().getKeymap().clickPlacesClips
                                    && ! e.mods.isCommandDown() && ! findCrossfadeAt (e.getPosition()).has_value();

    if (placesForSmartTool)
        currentTool = Tool::draw;

    switch (currentTool)
    {
        case Tool::draw:
        case Tool::paint:
        {
            // Draw drags clips as the Smart Tool does; Paint selects them.
            if (clip != nullptr)
            {
                if (currentTool == Tool::draw)
                {
                    smartToolDown (e);
                }
                else
                {
                    clipClicked (*clip, e.mods);
                    gesture = nullptr;
                }

                return;
            }

            range.clear();
            gesture->beginStep (*ed, currentTool == Tool::draw ? "Place Clip" : "Paint Clips");
            auto* placed = placeClip (gesture->startRow, gesture->startBeat, getGrid().snap && ! e.mods.isAltDown());

            if (placed == nullptr)
            {
                gesture->endStep(); // nothing was placed, so a drag that follows begins a step of its own

                // Nothing to place (a track without an instrument, before any clip was used): the Smart Tool's click.
                if (placesForSmartTool)
                {
                    smartToolDown (e);
                    return;
                }

                gesture = nullptr;
                return;
            }

            selection.selectOnly (placed);

            if (currentTool == Tool::draw)
            {
                // A drag that follows moves the new clip.
                gesture->kind = Gesture::Kind::move;
                gesture->clip = placed;
                gesture->anchorStart = placed->getStartBeat();
                gesture->moving.push_back ({ placed, placed->getStartBeat(), gesture->startRow });
            }
            else
            {
                gesture->kind = Gesture::Kind::paint;
                gesture->nextPaintStart = placed->getEndBeat();
            }

            return;
        }

        case Tool::slice:
            if (clip != nullptr)
            {
                gesture->beginStep (*ed, "Split Clip");
                splitClips ({ clip }, snap (gesture->startBeat, e.mods));
            }

            gesture = nullptr;
            return;

        case Tool::slip:
            if (clip == nullptr)
            {
                gesture = nullptr;
                return;
            }

            clipClicked (*clip, {});
            gesture->beginStep (*ed, "Slip Clip");
            gesture->kind = Gesture::Kind::slip;
            gesture->clip = clip;
            gesture->startOffset = clip->getPosition().getOffset();
            return;

        case Tool::mute:
        case Tool::erase:
            gesture->beginStep (*ed, currentTool == Tool::mute ? "Mute Clips" : "Delete Clips");
            gesture->kind = currentTool == Tool::mute ? Gesture::Kind::mute : Gesture::Kind::erase;
            muteOrDeleteClipAt (e.getPosition(), currentTool == Tool::erase);
            return;

        case Tool::select:
            // A click selects (when the mouse goes up); a drag draws a rectangle.
            gesture->clip = clip;
            gesture->clickOnMouseUp = true;
            return;

        case Tool::zoom:
            return; // a click or a drag, decided later

        case Tool::smart:
        default:
            smartToolDown (e);
            return;
    }
}

void Playlist::smartToolDown (const juce::MouseEvent& e)
{
    // Where two audio clips meet: a drag draws a crossfade.
    if (! e.mods.isPopupMenu())
    {
        if (auto spot = findCrossfadeAt (e.getPosition()))
        {
            gesture->crossfade = spot;
            return;
        }
    }

    // On a clip: where it is clicked decides what a drag does.
    if (auto* component = getClipComponentAt (e.getPosition()))
    {
        auto& clip = *component->getClip();
        gesture->clip = &clip;
        gesture->zone = getClipZone (getClipBounds (clip).toFloat(), e.position, dynamic_cast<te::AudioClipBase*> (&clip) != nullptr);

        if (e.mods.isPopupMenu())
        {
            if (! selection.isSelected (clip))
                clipClicked (clip, {});

            gesture = nullptr;
            showClipMenu();
            return;
        }

        // A plain click on a clip that isn't selected selects it at once, so a drag moves it. Other clicks
        // wait to see whether they become drags: a selected clip's drag moves the whole selection, and a
        // Ctrl-drag copies rather than selecting.
        if (! selection.isSelected (clip) && ! (e.mods.isCommandDown() || e.mods.isShiftDown()))
            clipClicked (clip, {});
        else
            gesture->clickOnMouseUp = true;

        return;
    }

    // On empty space: the track is selected, and a drag selects a time range.
    range.clear();

    if (gesture->startRow < 0)
    {
        selection.deselectAll();
        return;
    }

    if (auto* track = rows[(size_t) gesture->startRow].track.get())
    {
        if (e.mods.isPopupMenu())
        {
            if (! selection.isSelected (track))
                trackClicked (*track, {});

            gesture = nullptr;
            showTrackMenu (*track);
            return;
        }

        trackClicked (*track, e.mods);
    }
}

void Playlist::lanesMouseDrag (const juce::MouseEvent& e)
{
    auto* ed = getEdit();

    if (gesture == nullptr || ed == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        if (! TimelineGesture::isPastDragThreshold (e))
            return;

        beginDrag (e);
    }

    // Past an edge, the lanes scroll along, and the drag goes on from what is under the pointer then. Rows are
    // found where the pointer is, or at the edge it is past.
    if (const auto directions = gesture->getAutoScrollDirections())
        autoScroll (e.getPosition(), lanes->getLocalBounds().withTrimmedTop (pinnedHeight), *directions);

    const auto beat = getView().xToBeat (e.position.x);
    const auto rowAtPointer = getRowAt (juce::jlimit (0, lanes->getHeight() - 1, e.y));
    auto* clip = gesture->clip.get();

    switch (gesture->kind)
    {
        case Gesture::Kind::move:
        {
            // The clip under the mouse snaps to the grid, and the others move with it, but no clip moves
            // before the start of the song.
            auto delta = snap (gesture->anchorStart + (beat - gesture->startBeat), e.mods) - gesture->anchorStart;

            for (const auto& moving : gesture->moving)
                delta = std::max (delta, te::BeatPosition() - moving.start);

            // Up or down by whole rows, as long as every clip lands on a track that holds clips.
            if (const auto row = rowAtPointer; row >= 0 && gesture->startRow >= 0)
            {
                const auto offset = row - gesture->startRow;
                const auto fits = std::all_of (gesture->moving.begin(), gesture->moving.end(),
                                               [this, offset] (const Gesture::MovingClip& moving)
                                               {
                                                   const auto target = moving.row + offset;
                                                   return juce::isPositiveAndBelow (target, (int) rows.size())
                                                          && dynamic_cast<te::ClipTrack*> (rows[(size_t) target].track.get()) != nullptr;
                                               });

                if (fits)
                    gesture->rowOffset = offset;
            }

            for (const auto& moving : gesture->moving)
            {
                const auto row = moving.row + gesture->rowOffset;

                if (auto* c = moving.clip.get(); c != nullptr && juce::isPositiveAndBelow (row, (int) rows.size()))
                    if (auto* track = dynamic_cast<te::ClipTrack*> (rows[(size_t) row].track.get()))
                        moveClip (*c, moving.start + delta, *track);
            }

            break;
        }

        case Gesture::Kind::trimStart:
            if (clip != nullptr)
                trimClipStart (*clip, snap (beat, e.mods));

            break;

        case Gesture::Kind::trimEnd:
            if (clip != nullptr)
                trimClipEnd (*clip, snap (beat, e.mods));

            break;

        case Gesture::Kind::fadeIn:
        case Gesture::Kind::fadeOut:
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                const auto time = ed->tempoSequence.toTime (beat);
                const auto fadeIn = gesture->kind == Gesture::Kind::fadeIn;
                setClipFade (*audio, fadeIn, fadeIn ? time - audio->getPosition().getStart() : audio->getPosition().getEnd() - time);
            }

            break;

        case Gesture::Kind::selectRange:
        {
            // From where the drag started to the pointer, across the rows between them.
            const auto lastRow = (int) rows.size() - 1;
            const auto from = snap (gesture->startBeat, e.mods), to = snap (beat, e.mods);
            const auto row = rowAtPointer >= 0 ? rowAtPointer : lastRow; // below the last row: up to it

            if (lastRow >= 0 && from != to)
                selectRange (from, to, gesture->startRow >= 0 ? gesture->startRow : lastRow, row);

            break;
        }

        case Gesture::Kind::crossfade:
        {
            // As long as twice the distance from the boundary to the mouse: the boundary stays in the middle.
            const auto& spot = *gesture->crossfade;
            auto* first = dynamic_cast<te::AudioClipBase*> (spot.first.get());
            auto* second = dynamic_cast<te::AudioClipBase*> (spot.second.get());

            if (first != nullptr && second != nullptr)
            {
                const auto distance = ed->tempoSequence.toTime (beat) - spot.boundary;
                setCrossfade (*first, *second, spot.boundary, te::TimeDuration::fromSeconds (2.0 * std::abs (distance.inSeconds())));
            }

            break;
        }

        case Gesture::Kind::slip:
            if (clip != nullptr)
            {
                // What is inside follows the mouse; the clip stays where it is.
                const auto& tempo = ed->tempoSequence;
                slipClip (*clip, gesture->startOffset,
                          tempo.toTime (snap (beat, e.mods)) - tempo.toTime (snap (gesture->startBeat, e.mods)));
            }

            break;

        case Gesture::Kind::paint:
            // Another copy each time the mouse passes the end of the last one.
            while (beat >= gesture->nextPaintStart)
            {
                auto* placed = placeClip (gesture->startRow, gesture->nextPaintStart, false);

                if (placed == nullptr)
                    break;

                selection.addToSelection (placed);
                gesture->nextPaintStart = placed->getEndBeat();
            }

            break;

        case Gesture::Kind::mute:
        case Gesture::Kind::erase:
            muteOrDeleteClipAt (e.getPosition(), gesture->kind == Gesture::Kind::erase);
            break;

        case Gesture::Kind::select:
            rubberBand = juce::Rectangle<int> (getGestureStart(), e.getPosition());
            selectClipsIn (rubberBand, e.mods.isCommandDown());
            lanes->repaint();
            break;

        case Gesture::Kind::zoom:
            rubberBand = juce::Rectangle<int> (getGestureStart(), e.getPosition()).withY (0).withHeight (lanes->getHeight());
            lanes->repaint();
            break;

        case Gesture::Kind::pending:
        default:
            break;
    }
}

void Playlist::beginDrag (const juce::MouseEvent& e)
{
    auto* ed = getEdit();
    auto* clip = gesture->clip.get();
    gesture->clickOnMouseUp = false;

    if (getTool() == Tool::select)
    {
        gesture->kind = Gesture::Kind::select;

        if (e.mods.isCommandDown())
            for (auto* item : selection.getSelectedObjects())
                gesture->selectedBefore.emplace_back (item);

        return;
    }

    if (getTool() == Tool::zoom)
    {
        gesture->kind = Gesture::Kind::zoom;
        return;
    }

    if (gesture->crossfade.has_value())
    {
        gesture->kind = Gesture::Kind::crossfade;
        gesture->beginStep (*ed, "Crossfade");
        return;
    }

    if (clip == nullptr || ! gesture->zone.has_value() || *gesture->zone == ClipZone::selectRange)
    {
        // The range is the selection now. (Nothing else stays selected, so Delete can't take a whole track
        // when a range was meant.)
        gesture->kind = Gesture::Kind::selectRange;
        selection.deselectAll();
        return;
    }

    switch (*gesture->zone)
    {
        case ClipZone::trimStart:
            gesture->kind = Gesture::Kind::trimStart;
            gesture->beginStep (*ed, "Trim Clip Start");
            return;

        case ClipZone::trimEnd:
            gesture->kind = Gesture::Kind::trimEnd;
            gesture->beginStep (*ed, "Trim Clip End");
            return;

        case ClipZone::fadeIn:
            gesture->kind = Gesture::Kind::fadeIn;
            gesture->beginStep (*ed, "Fade In");
            return;

        case ClipZone::fadeOut:
            gesture->kind = Gesture::Kind::fadeOut;
            gesture->beginStep (*ed, "Fade Out");
            return;

        case ClipZone::move:
        case ClipZone::selectRange:
        default:
            break;
    }

    // Moving: the clip under the mouse goes with the rest of the selection. With Ctrl, copies of them
    // move instead, and with Ctrl and Shift, aliases (copies that stay linked to their originals).
    gesture->kind = Gesture::Kind::move;

    if (! selection.isSelected (clip))
        selection.addToSelection (clip);

    auto clips = getSelectedClips (selection);
    const auto alias = e.mods.isCommandDown() && e.mods.isShiftDown();
    const auto copy = e.mods.isCommandDown() && ! alias;
    const auto many = clips.size() > 1;
    gesture->beginStep (*ed, alias  ? (many ? "Make Aliases" : "Make Alias")
                             : copy ? (many ? "Copy Clips" : "Copy Clip")
                                    : (many ? "Move Clips" : "Move Clip"));

    if (copy || alias)
    {
        const auto anchorIndex = clips.indexOf (clip);
        clips = copyClips (clips, alias);
        clip = clips[anchorIndex];
        gesture->clip = clip;

        selection.deselectAll(); // the copies are what moves

        for (auto* c : clips)
            selection.addToSelection (c);
    }

    gesture->anchorStart = clip != nullptr ? clip->getStartBeat() : gesture->startBeat;

    for (auto* c : clips)
        gesture->moving.push_back ({ c, c->getStartBeat(), getRowOf (c->getTrack()) });
}

juce::Point<int> Playlist::getGestureStart() const
{
    return gesture->getStart (getView(), scrollY, pinnedHeight); // the pinned band doesn't scroll up and down
}

void Playlist::lanesMouseUp (const juce::MouseEvent& e)
{
    if (gesture == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        // A click, not a drag.
        if (getTool() == Tool::zoom)
        {
            zoomAround (e.position.x, e.mods.isAltDown() ? 0.5 : 2.0);
        }
        else if (auto* clip = gesture->clip.get(); clip != nullptr && gesture->clickOnMouseUp)
        {
            clipClicked (*clip, e.mods);
        }
        else if (getTool() == Tool::select && ! e.mods.isCommandDown())
        {
            range.clear();
            selection.deselectAll();
        }
    }
    else if (gesture->kind == Gesture::Kind::zoom && rubberBand.getWidth() > TimelineGesture::dragThreshold)
    {
        // Zoom to the dragged time.
        zoomTo ({ getView().xToBeat (rubberBand.getX()), getView().xToBeat (rubberBand.getRight()) });
    }

    gesture = nullptr;

    if (! rubberBand.isEmpty())
    {
        rubberBand = {};
        lanes->repaint();
    }

    lanesMouseMove (e);
}

void Playlist::lanesDoubleClick (const juce::MouseEvent& e)
{
    auto* ed = getEdit();
    const auto row = getRowAt (e.y);

    if (ed == nullptr || getTool() != Tool::smart)
        return;

    // Under the last track: a new one at the end, as in Logic, Cubase and REAPER.
    if (row < 0)
    {
        if (e.y >= pinnedHeight + contentHeight - scrollY)
            addTrackAtEnd (false);

        return;
    }

    // On a MIDI clip: open it in the piano roll.
    if (auto* component = getClipComponentAt (e.getPosition()))
    {
        if (auto* midi = dynamic_cast<te::MidiClip*> (component->getClip()); midi != nullptr && onOpenClip != nullptr)
            onOpenClip (*midi);

        return;
    }

    // On a track with an instrument: an empty MIDI clip, a bar long, from the grid line before the click.
    auto* track = dynamic_cast<te::AudioTrack*> (rows[(size_t) row].track.get());

    if (track == nullptr || ! hasInstrument (*track))
        return;

    addMidiClipAt (*track, snapDown (getView().xToBeat (e.position.x), e.mods), &selection);
}
} // namespace phazite::ui
