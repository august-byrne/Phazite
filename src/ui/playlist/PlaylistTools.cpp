// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tools: what the mouse does in the lanes (ui/timeline/SmartTool.h), and the edits of the selected clips
// that the time line's commands make.
#include "PlaylistParts.h"
#include "model/arrangement/TimelineModel.h"
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
void Playlist::clipClicked (ClipID clip, const juce::ModifierKeys& mods)
{
    range.clear();

    // Ctrl or Shift adds the clip to the selection, or takes it out.
    if (! (mods.isCommandDown() || mods.isShiftDown()))
        selection.selectOnly (clip);
    else if (selection.isSelected (clip))
        selection.remove (clip);
    else
        selection.add (clip);
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

void Playlist::selectRange (BeatPosition from, BeatPosition to, int fromRow, int toRow)
{
    std::vector<TrackID> tracks;

    for (auto row = std::min (fromRow, toRow); row <= std::max (fromRow, toRow); ++row)
        if (juce::isPositiveAndBelow (row, (int) rows.size()))
            tracks.push_back (rows[(size_t) row].track.id);

    range.set (BeatRange::between (from, to), tracks);
}

std::optional<CrossfadeSpot> Playlist::findCrossfadeAt (juce::Point<int> position) const
{
    const auto row = getRowAt (position.y);

    if (row < 0)
        return {};

    // Only the lower third of a row, so the upper part still trims.
    const auto& r = rows[(size_t) row];

    if (position.y < getRowTop (r) + r.height * 2 / 3)
        return {};

    // Clips a pixel apart count as touching.
    const auto pixel = BeatDuration::fromBeats (1.0 / getView().pixelsPerBeat);

    for (const auto& spot : getCrossfadeSpots (context, r.track.id, pixel))
        if (std::abs (getView().beatToX (spot.boundary) - position.x) <= 6.0)
            return spot;

    return {};
}

ClipID Playlist::placeClip (int row, BeatPosition beat, bool snapStart)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return {};

    // From the grid line at or before the beat.
    return phazite::placeClip (context, rows[(size_t) row].track.id, snapStart ? snapDown (beat, {}) : beat, lastClip);
}

void Playlist::muteOrDeleteClipAt (juce::Point<int> position, bool deleteIt)
{
    auto* component = getClipComponentAt (position);

    if (component == nullptr || gesture == nullptr)
        return;

    // Each clip once per drag, however long the mouse stays on it.
    const auto clip = component->getClip();

    if (! gesture->doOnce (clip))
        return;

    if (deleteIt)
        deleteClips (context, { clip }, false);
    else
        toggleClipsMuted (context, { clip });
}

void Playlist::selectClipsIn (juce::Rectangle<int> area, bool addToSelection)
{
    range.clear();
    selection.clear();

    if (addToSelection && gesture != nullptr)
    {
        for (const auto track : gesture->tracksBefore)
            selection.add (track);

        for (const auto clip : gesture->clipsBefore)
            selection.add (clip);

        for (const auto section : gesture->sectionsBefore)
            selection.add (section);
    }

    for (auto* component : clipComponents)
        if (getClipBounds (component->getInfo()).intersects (area))
            selection.add (component->getClip());
}

//==============================================================================
bool Playlist::canNudgeSelection() const
{
    return ! range.isEmpty() || ! selection.getClips().empty() || hasSelectedAutomationPoints (context);
}

void Playlist::nudgeSelection (int direction)
{
    // A step of the grid as it is now, so what is seen is what moves.
    const auto step = BeatDuration::fromBeats (direction * getGridStepInBeats());

    if (hasSelectedAutomationPoints (context))
    {
        moveSelectedAutomationPoints (context, step, 0.0f);
        return;
    }

    if (const auto clips = selection.getClips(); ! clips.empty())
    {
        nudgeClips (context, clips, step);
        return;
    }

    if (! range.isEmpty())
    {
        const auto r = range.getRange();
        const auto start = BeatPosition::fromBeats (std::max (0.0, (r.getStart() + step).inBeats()));
        range.set ({ start, r.getLength() }, range.getTracks());
    }
}

bool Playlist::canQuantizeSelection() const
{
    return ! selection.getClips().empty();
}

void Playlist::quantizeSelection()
{
    // To the grid as it is now, whether or not snapping is on: quantizing is asked for explicitly.
    for (const auto id : selection.getClips())
        if (const auto clip = getClip (context, id))
            moveClip (context, id, snapToGrid (context, clip->range.getStart(), getGridStep()), clip->track);
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
        const auto& clip = component->getInfo();
        const auto zone = getClipZone (getClipBounds (clip).toFloat(), e.position, clip.kind == ClipType::audio);
        const auto addsPoints =
            getCurveOf (*component) != nullptr && currentTool == Tool::smart && (zone == ClipZone::move || zone == ClipZone::selectRange);
        lanes->setMouseCursor (getCursorFor (zone));
        lanes->setDescription (describeClipZone (zone) + (addsPoints ? " Double-click to add a point to its curve." : ""));
        return;
    }

    lanes->setMouseCursor (getCursorFor (currentTool));
    lanes->setDescription (currentTool == Tool::smart ? emptySpaceHint : describeTool (currentTool));
}

void Playlist::lanesMouseDown (const juce::MouseEvent& e)
{
    gesture = nullptr;

    if (! context.hasProject())
        return;

    // The master's row holds no clips, only the lanes under it.
    if (const auto row = getRowAt (e.y); row >= 0 && rows[(size_t) row].track.kind == TrackType::master && getTool() != Tool::zoom)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startPosition = e.getPosition();
    gesture->startBeat = getView().xToBeat (e.position.x);
    gesture->startRow = getRowAt (e.y);
    gesture->startScrollY = scrollY;
    beginDragAutoRepeat (autoRepeatInterval); // so the lanes go on scrolling while the mouse rests past an edge

    auto* component = getClipComponentAt (e.getPosition());
    const auto clip = component != nullptr ? component->getClip() : ClipID();
    auto currentTool = getTool();

    if (e.mods.isPopupMenu())
    {
        // In FL Studio's keymap, right-click deletes, as in FL Studio: this clip and those the mouse moves on to.
        // Shift+right-click opens the menu, as in the piano roll.
        if (getCommands().getKeymap().rightClickDeletes && ! e.mods.isShiftDown())
        {
            gesture->beginStep (context, "Delete Clips");
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
    const auto placesForSmartTool = currentTool == Tool::smart && ! clip.isValid() && getCommands().getKeymap().clickPlacesClips
                                    && ! e.mods.isCommandDown() && ! findCrossfadeAt (e.getPosition()).has_value();

    if (placesForSmartTool)
        currentTool = Tool::draw;

    switch (currentTool)
    {
        case Tool::draw:
        case Tool::paint:
        {
            // Draw drags clips as the Smart Tool does; Paint selects them.
            if (clip.isValid())
            {
                if (currentTool == Tool::draw)
                {
                    smartToolDown (e);
                }
                else
                {
                    clipClicked (clip, e.mods);
                    gesture = nullptr;
                }

                return;
            }

            range.clear();
            gesture->beginStep (context, currentTool == Tool::draw ? "Place Clip" : "Paint Clips");
            const auto placed = placeClip (gesture->startRow, gesture->startBeat, getGrid().snap && ! e.mods.isAltDown());
            const auto placedClip = getClip (context, placed);

            if (! placedClip.has_value())
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
                gesture->anchorStart = placedClip->range.getStart();
                gesture->moving.push_back ({ placed, placedClip->range.getStart(), gesture->startRow });
            }
            else
            {
                gesture->kind = Gesture::Kind::paint;
                gesture->nextPaintStart = placedClip->range.getEnd();
            }

            return;
        }

        case Tool::slice:
            if (clip.isValid())
            {
                gesture->beginStep (context, "Split Clip");
                splitClips (context, { clip }, snap (gesture->startBeat, e.mods));
            }

            gesture = nullptr;
            return;

        case Tool::slip:
            if (! clip.isValid())
            {
                gesture = nullptr;
                return;
            }

            clipClicked (clip, {});
            gesture->beginStep (context, "Slip Clip");
            gesture->kind = Gesture::Kind::slip;
            gesture->clip = clip;
            gesture->startOffset = getSlipOffset (context, clip);
            return;

        case Tool::mute:
        case Tool::erase:
            gesture->beginStep (context, currentTool == Tool::mute ? "Mute Clips" : "Delete Clips");
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
        const auto& info = component->getInfo();
        const auto clip = info.id;
        gesture->clip = clip;
        gesture->zone = getClipZone (getClipBounds (info).toFloat(), e.position, info.kind == ClipType::audio);

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
        selection.clear();
        return;
    }

    const auto track = rows[(size_t) gesture->startRow].track.id;

    if (e.mods.isPopupMenu())
    {
        if (! selection.isSelected (track))
            trackClicked (track, {});

        gesture = nullptr;
        showTrackMenu (track);
        return;
    }

    trackClicked (track, e.mods);
}

void Playlist::lanesMouseDrag (const juce::MouseEvent& e)
{
    if (gesture == nullptr || ! context.hasProject())
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
    const auto clip = gesture->clip;

    switch (gesture->kind)
    {
        case Gesture::Kind::move:
        {
            // The clip under the mouse snaps to the grid, and the others move with it, but no clip moves
            // before the start of the song.
            auto delta = snap (gesture->anchorStart + (beat - gesture->startBeat), e.mods) - gesture->anchorStart;

            for (const auto& moving : gesture->moving)
                delta = std::max (delta, BeatPosition() - moving.start);

            // Up or down by whole rows, as long as every clip lands on a track that holds clips (not a folder, nor the
            // master's row).
            if (const auto row = rowAtPointer; row >= 0 && gesture->startRow >= 0)
            {
                const auto offset = row - gesture->startRow;
                const auto fits = std::all_of (gesture->moving.begin(), gesture->moving.end(),
                                               [this, offset] (const Gesture::MovingClip& moving)
                                               {
                                                   const auto target = moving.row + offset;
                                                   return juce::isPositiveAndBelow (target, (int) rows.size())
                                                          && canHoldClips (context, rows[(size_t) target].track.id);
                                               });

                if (fits)
                    gesture->rowOffset = offset;
            }

            for (const auto& moving : gesture->moving)
                if (const auto row = moving.row + gesture->rowOffset; juce::isPositiveAndBelow (row, (int) rows.size()))
                    moveClip (context, moving.clip, moving.start + delta, rows[(size_t) row].track.id);

            break;
        }

        case Gesture::Kind::trimStart:
            trimClipStart (context, clip, snap (beat, e.mods));
            break;

        case Gesture::Kind::trimEnd:
            trimClipEnd (context, clip, snap (beat, e.mods));
            break;

        case Gesture::Kind::fadeIn:
        case Gesture::Kind::fadeOut:
            fadeClipTo (context, clip, gesture->kind == Gesture::Kind::fadeIn, beat);
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
            setCrossfade (context, *gesture->crossfade, beat);
            break;

        case Gesture::Kind::slip:
            // What is inside follows the mouse; the clip stays where it is.
            slipClip (context, clip, gesture->startOffset, snap (gesture->startBeat, e.mods), snap (beat, e.mods));
            break;

        case Gesture::Kind::paint:
            // Another copy each time the mouse passes the end of the last one.
            while (beat >= gesture->nextPaintStart)
            {
                const auto placed = getClip (context, placeClip (gesture->startRow, gesture->nextPaintStart, false));

                if (! placed.has_value())
                    break;

                selection.add (placed->id);
                gesture->nextPaintStart = placed->range.getEnd();
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
    auto clip = gesture->clip;
    gesture->clickOnMouseUp = false;

    if (getTool() == Tool::select)
    {
        gesture->kind = Gesture::Kind::select;

        if (e.mods.isCommandDown())
        {
            gesture->tracksBefore = selection.getTracks();
            gesture->clipsBefore = selection.getClips();
            gesture->sectionsBefore = selection.getSections();
        }

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
        gesture->beginStep (context, "Crossfade");
        return;
    }

    if (! clip.isValid() || ! gesture->zone.has_value() || *gesture->zone == ClipZone::selectRange)
    {
        // The range is the selection now. (Nothing else stays selected, so Delete can't take a whole track
        // when a range was meant.)
        gesture->kind = Gesture::Kind::selectRange;
        selection.clear();
        return;
    }

    switch (*gesture->zone)
    {
        case ClipZone::trimStart:
            gesture->kind = Gesture::Kind::trimStart;
            gesture->beginStep (context, "Trim Clip Start");
            return;

        case ClipZone::trimEnd:
            gesture->kind = Gesture::Kind::trimEnd;
            gesture->beginStep (context, "Trim Clip End");
            return;

        case ClipZone::fadeIn:
            gesture->kind = Gesture::Kind::fadeIn;
            gesture->beginStep (context, "Fade In");
            return;

        case ClipZone::fadeOut:
            gesture->kind = Gesture::Kind::fadeOut;
            gesture->beginStep (context, "Fade Out");
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
        selection.add (clip);

    auto clips = selection.getClips();
    const auto alias = e.mods.isCommandDown() && e.mods.isShiftDown();
    const auto copy = e.mods.isCommandDown() && ! alias;
    const auto many = clips.size() > 1;
    gesture->beginStep (context, alias  ? (many ? "Make Aliases" : "Make Alias")
                                 : copy ? (many ? "Copy Clips" : "Copy Clip")
                                        : (many ? "Move Clips" : "Move Clip"));

    if (copy || alias)
    {
        const auto anchorIndex = (size_t) (std::find (clips.begin(), clips.end(), clip) - clips.begin());
        clips = copyClips (context, clips, alias);
        clip = anchorIndex < clips.size() ? clips[anchorIndex] : ClipID();
        gesture->clip = clip;

        selection.clear(); // the copies are what moves

        for (const auto c : clips)
            selection.add (c);
    }

    const auto anchor = getClip (context, clip);
    gesture->anchorStart = anchor.has_value() ? anchor->range.getStart() : gesture->startBeat;

    for (const auto id : clips)
        if (const auto moving = getClip (context, id))
            gesture->moving.push_back ({ id, moving->range.getStart(), getRowOf (moving->track) });
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
        else if (gesture->clip.isValid() && gesture->clickOnMouseUp)
        {
            clipClicked (gesture->clip, e.mods);
        }
        else if (getTool() == Tool::select && ! e.mods.isCommandDown())
        {
            range.clear();
            selection.clear();
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
    const auto row = getRowAt (e.y);

    if (! context.hasProject() || getTool() != Tool::smart)
        return;

    // Under the last track: a new one at the end, as in Logic, Cubase and REAPER.
    if (row < 0)
    {
        if (e.y >= pinnedHeight + contentHeight - scrollY)
            addTrackAtEnd (false);

        return;
    }

    // On a MIDI clip: open it in the piano roll. On an automation clip: a point on its curve there.
    if (auto* component = getClipComponentAt (e.getPosition()))
    {
        if (component->getInfo().kind == ClipType::midi && onOpenClip != nullptr)
            onOpenClip (component->getClip());
        else if (auto* curve = getCurveOf (*component))
            if (const auto inCurve = curve->getLocalPoint (lanes.get(), e.position); curve->getLocalBounds().toFloat().contains (inCurve))
                curve->addPointAt (inCurve, e.mods);

        return;
    }

    // On a track with an instrument: an empty MIDI clip, a bar long, from the grid line before the click.
    const auto track = getTrackRow (context, rows[(size_t) row].track.id);

    if (! track.has_value() || track->kind != TrackType::track || ! track->hasInstrument)
        return;

    addMidiClipAt (context, track->id, snapDown (getView().xToBeat (e.position.x), e.mods));
}
} // namespace phazite::ui
