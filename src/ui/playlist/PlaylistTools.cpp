// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tools: what the mouse does in the lanes (ui/timeline/SmartTool.h), and the edits of the selected clips
// that the time line's commands make.
#include "PlaylistParts.h"
#include "model/arrangement/TimelineModel.h"
#include "model/transport/RecordingModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <map>

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
    addCommandItems (menu, bridge, { "edit.cut", "edit.copy", "edit.paste" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "edit.split", "edit.duplicate", "edit.duplicateAsAlias", "clip.makeUnique", "clip.consolidate" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "edit.mute", "edit.quantize" });

    juce::PopupMenu fadeShapes;
    addCommandItems (fadeShapes, bridge, { "clip.fadeLinear", "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve" });
    menu.addSubMenu ("Fade Shape", fadeShapes);
    addCommandItems (menu, bridge, { "clip.reverse", "clip.followTempo", "clip.quantizeAudio", "clip.normalize" });

    // A clip recorded round a looped range: its takes, the one it plays ticked, each one undo step (RecordingModel.h).
    for (const auto id : selection.getClips())
    {
        if (const auto takes = getClipTakes (context, id); takes.count > 1)
        {
            juce::PopupMenu takeMenu;

            for (int take = 1; take <= takes.count; ++take)
                takeMenu.addItem ("Take " + juce::String (take), true, take == takes.playing,
                                  [&projectContext = context, id, take] { setClipTake (projectContext, id, take); });

            takeMenu.addSeparator();
            addCommandItems (takeMenu, bridge, { "clip.previousTake", "clip.nextTake" });
            menu.addSubMenu ("Take", takeMenu);
            break;
        }
    }

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

std::optional<CrossfadeSpot> Playlist::findCrossfadeAt (juce::Point<int> position)
{
    const auto row = getRowAt (position.y);

    if (row < 0)
        return {};

    // The track's spots, as the model finds them once for the clips and the zoom as they are: clips a pixel apart count as
    // touching.
    if (! juce::exactlyEqual (crossfadeZoom, getView().pixelsPerBeat))
    {
        crossfadeSpots.clear();
        crossfadeZoom = getView().pixelsPerBeat;
    }

    const auto track = rows[(size_t) row].track.id;
    auto known = crossfadeSpots.find (track);

    if (known == crossfadeSpots.end())
        known = crossfadeSpots.emplace (track, getCrossfadeSpots (context, track, BeatDuration::fromBeats (1.0 / crossfadeZoom))).first;

    for (const auto& spot : known->second)
    {
        // Only where the two show side by side, in the same sub-row (clips that overlap show one under another), and in
        // the lower third of it, so the upper part still trims.
        const auto first = clipIndex.find (spot.first), second = clipIndex.find (spot.second);

        if (first == clipIndex.end() || second == clipIndex.end() || shownSubRows[first->second] != shownSubRows[second->second])
            continue;

        const auto bounds = getClipBounds (shownClips[first->second]);

        if (position.y >= bounds.getY() + bounds.getHeight() * 2 / 3 && position.y <= bounds.getBottom()
            && std::abs (getView().beatToX (spot.boundary) - position.x) <= 6.0)
            return spot;
    }

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
    const auto* hit = getClipAt (position);

    if (hit == nullptr || gesture == nullptr)
        return;

    // Each clip once per drag, however long the mouse stays on it.
    const auto clip = hit->id;

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

    // Every clip there, those scrolled out of view too.
    for (const auto& clip : shownClips)
        if (getClipBounds (clip).intersects (area))
            selection.add (clip.id);
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

std::optional<BeatRange> Playlist::getSelectedRange() const
{
    // A time range selected across tracks, or else the selected clips, from the first's start to the last's end.
    if (! range.isEmpty())
        return range.getRange();

    std::optional<BeatRange> shown;

    for (const auto id : selection.getClips())
        if (const auto clip = getClip (context, id))
            shown = shown.has_value()
                        ? BeatRange (std::min (shown->getStart(), clip->range.getStart()), std::max (shown->getEnd(), clip->range.getEnd()))
                        : clip->range;

    return shown;
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
    setDragReadout ({});
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

    const auto* clip = getClipAt (e.getPosition());

    if (clip != nullptr && (currentTool == Tool::smart || currentTool == Tool::draw))
    {
        const auto zone = getClipZone (getClipBounds (*clip).toFloat(), e.position, clip->kind == ClipType::audio);
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
    setDragReadout ({});

    if (! context.hasProject())
        return;

    // The middle button moves the view, as a page by hand, as it does in the piano roll; it never edits.
    if (e.mods.isMiddleButtonDown())
    {
        gesture = std::make_unique<Gesture>();
        gesture->kind = Gesture::Kind::pan;
        gesture->startPosition = e.getPosition();
        gesture->panViewStart = getView().start;
        gesture->startScrollY = scrollY;
        return;
    }

    // The master's row holds no clips, only the lanes under it.
    if (const auto row = getRowAt (e.y); row >= 0 && rows[(size_t) row].track.kind == TrackType::master && getTool() != Tool::zoom)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startPosition = e.getPosition();
    gesture->startBeat = getView().xToBeat (e.position.x);
    gesture->startRow = getRowAt (e.y);
    gesture->startScrollY = scrollY;
    gesture->selectionAtPress = SelectionAtPress::take (context);
    beginDragAutoRepeat (autoRepeatInterval); // so the lanes go on scrolling while the mouse rests past an edge

    const auto* hit = getClipAt (e.getPosition());
    const auto clip = hit != nullptr ? hit->id : ClipID();
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
    if (const auto* hit = getClipAt (e.getPosition()))
    {
        const auto clip = hit->id;
        gesture->clip = clip;
        gesture->zone = getClipZone (getClipBounds (*hit).toFloat(), e.position, hit->kind == ClipType::audio);

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

    if (gesture->kind == Gesture::Kind::pan)
    {
        // The song moves with the pointer, sideways and up and down; while it plays, following pauses, as a scroll pauses it.
        pauseFollowing();
        setViewStart (gesture->panViewStart - BeatDuration::fromBeats (e.getDistanceFromDragStartX() / getView().pixelsPerBeat));
        setScrollY (gesture->startScrollY - e.getDistanceFromDragStartY());
        return;
    }

    if (gesture->kind == Gesture::Kind::pending)
    {
        if (! TimelineGesture::isPastDragThreshold (e))
            return;

        beginDrag (e);
    }

    // Past an edge, the lanes scroll along, and the drag goes on from what is under the pointer then. Rows are
    // found where the pointer is, or at the edge it is past.
    if (const auto directions = gesture->getAutoScrollDirections())
        if (autoScroll (e, getTimelineArea().withZeroOrigin().withTrimmedTop (pinnedHeight), *directions))
            clipsBehind = true; // until the change below reaches the playlist (frameComing())

    const auto beat = getView().xToBeat (e.position.x);
    const auto rowAtPointer = getRowAt (juce::jlimit (0, lanes->getHeight() - 1, e.y));
    const auto clip = gesture->clip;

    switch (gesture->kind)
    {
        case Gesture::Kind::move:
        {
            // The clips move by the grid's steps from where they were, so that a clip off the grid stays as far off it, and a
            // drag to another track alone moves none in time (TimelineEditor::snapMove()); no clip moves before the start of
            // the song.
            auto delta = snapMove (gesture->anchorStart, beat - gesture->startBeat, e.mods);

            for (const auto& moving : gesture->moving)
                delta = std::max (delta, BeatPosition() - moving.start);

            // Up or down by whole rows, as long as every clip lands on a track that can hold it: not a folder, a return or the
            // master's row, and of its kind (audio and notes don't share a track: model/arrangement/PlaylistModel.h,
            // canPlaceClips()). Where they can't go, they stay on the rows they can, and the hint bar says why.
            gesture->refusal.clear();

            if (const auto row = rowAtPointer; row >= 0 && gesture->startRow >= 0)
            {
                const auto offset = row - gesture->startRow;
                std::map<int, std::vector<ClipID>> byTarget;
                std::vector<ClipID> leaving;
                auto fits = true;

                for (const auto& moving : gesture->moving)
                {
                    leaving.push_back (moving.clip);
                    const auto target = moving.row + offset;
                    fits = fits && juce::isPositiveAndBelow (target, (int) rows.size());
                    byTarget[target].push_back (moving.clip);
                }

                for (const auto& [target, clips] : byTarget)
                {
                    if (! fits)
                        break;

                    gesture->refusal = whyClipsCantGo (context, rows[(size_t) target].track.id, clips, leaving);
                    fits = gesture->refusal.isEmpty();
                }

                if (fits)
                    gesture->rowOffset = offset;
            }

            // Copies are made as the drag first takes them somewhere: until then, nothing changes. Back where they began, they go
            // again, rather than lying on their originals, playing twice, until the mouse goes up (known bug 21 until
            // 2026-11-14).
            if (gesture->copies && gesture->copying && delta == BeatDuration() && gesture->rowOffset == 0)
            {
                removeCopies();
                gesture->moved = {};
                break;
            }

            if (gesture->copies && ! gesture->copying)
            {
                if (delta == BeatDuration() && gesture->rowOffset == 0)
                    break;

                copyMovingClips();
            }

            for (const auto& moving : gesture->moving)
                if (const auto row = moving.row + gesture->rowOffset; juce::isPositiveAndBelow (row, (int) rows.size()))
                    moveClip (context, moving.clip, moving.start + delta, rows[(size_t) row].track.id);

            gesture->moved = delta;
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

        case Gesture::Kind::pan:
        case Gesture::Kind::pending:
        default:
            break;
    }

    setDragReadout (describeDrag());
}

juce::String Playlist::describeDrag() const
{
    // Where the clip under the pointer is now: its start, and its track while clips move between tracks; or the edge trimmed,
    // with the clip's length.
    const auto clip = gesture != nullptr ? getClip (context, gesture->clip) : std::nullopt;

    if (! clip.has_value())
        return {};

    const auto start = clip->range.getStart(), end = clip->range.getEnd();

    switch (gesture->kind)
    {
        case Gesture::Kind::move:
        {
            const auto track = getTrackRow (context, clip->track);
            return "Starts at " + describePlace (start) + (track.has_value() && gesture->rowOffset != 0 ? " on " + track->name : "")
                   + (gesture->refusal.isNotEmpty() ? " (" + gesture->refusal + ")" : "");
        }

        case Gesture::Kind::trimStart:
            return "Starts at " + describeSpan (start, end);

        case Gesture::Kind::trimEnd:
            return "Ends at " + describePlace (end) + ", " + describeLength (start, end);

        case Gesture::Kind::pending:
        case Gesture::Kind::fadeIn:
        case Gesture::Kind::fadeOut:
        case Gesture::Kind::selectRange:
        case Gesture::Kind::crossfade:
        case Gesture::Kind::slip:
        case Gesture::Kind::paint:
        case Gesture::Kind::mute:
        case Gesture::Kind::erase:
        case Gesture::Kind::select:
        case Gesture::Kind::zoom:
        case Gesture::Kind::pan:
        default:
            return {};
    }
}

void Playlist::beginDrag (const juce::MouseEvent& e)
{
    const auto clip = gesture->clip;
    const auto waitedForClick = gesture->clickOnMouseUp;
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
    // move instead, and with Ctrl and Shift, aliases (copies that stay linked to their originals): made once the drag moves
    // them (copyMovingClips()), so that a Ctrl+click whose pointer slipped a few pixels leaves no copies on the clips.
    gesture->kind = Gesture::Kind::move;
    gesture->pressed = clip;
    gesture->waitedForClick = waitedForClick;
    gesture->tracksBefore = selection.getTracks();
    gesture->clipsBefore = selection.getClips();
    gesture->sectionsBefore = selection.getSections();

    if (! selection.isSelected (clip))
        selection.add (clip);

    const auto clips = selection.getClips();
    gesture->aliases = e.mods.isCommandDown() && e.mods.isShiftDown();
    gesture->copies = e.mods.isCommandDown();
    const auto many = clips.size() > 1;
    gesture->stepIsTheDrags = ! gesture->hasStep();
    gesture->beginStep (context, gesture->aliases  ? (many ? "Make Aliases" : "Make Alias")
                                 : gesture->copies ? (many ? "Copy Clips" : "Copy Clip")
                                                   : (many ? "Move Clips" : "Move Clip"));
    setMovingClips (clips);
}

void Playlist::setMovingClips (const std::vector<ClipID>& clips)
{
    const auto anchor = getClip (context, gesture->clip);
    gesture->anchorStart = anchor.has_value() ? anchor->range.getStart() : gesture->startBeat;
    gesture->moving.clear();

    for (const auto id : clips)
        if (const auto moving = getClip (context, id))
            gesture->moving.push_back ({ id, moving->range.getStart(), getRowOf (moving->track), { moving->track, moving->range } });
}

void Playlist::copyMovingClips()
{
    // Copies of the clips where they are, which are what is selected and moves from here on.
    std::vector<ClipID> clips;

    for (const auto& moving : gesture->moving)
        clips.push_back (moving.clip);

    const auto anchorIndex = (size_t) (std::find (clips.begin(), clips.end(), gesture->clip) - clips.begin());
    gesture->clipsBeforeCopies = selection.getClips();
    clips = copyClips (context, clips, gesture->aliases);
    gesture->clip = anchorIndex < clips.size() ? clips[anchorIndex] : ClipID();
    gesture->copying = true;

    selection.clear();

    for (const auto c : clips)
        selection.add (c);

    setMovingClips (clips);
}

void Playlist::removeCopies()
{
    // In the drag's step, which a drag that ends where it began takes back whole; the originals move again from here on.
    std::vector<ClipID> copies;

    for (const auto& moving : gesture->moving)
        copies.push_back (moving.clip);

    deleteClips (context, copies, false);
    gesture->copying = false;
    gesture->clip = gesture->pressed;
    selection.clear();

    for (const auto clip : gesture->clipsBeforeCopies)
        selection.add (clip);

    setMovingClips (gesture->clipsBeforeCopies);
}

void Playlist::endMoveThatWentNowhere (const juce::ModifierKeys& mods)
{
    // The clips are where they began: a step the drag began is taken back, leaving nothing to undo or redo, and copies made
    // on the way go, which would lie on their originals and play twice.
    if (gesture->stepIsTheDrags)
    {
        gesture->rollBack();
    }
    else if (gesture->copying)
    {
        std::vector<ClipID> copies;

        for (const auto& moving : gesture->moving)
            copies.push_back (moving.clip);

        deleteClips (context, copies, false);
    }

    if (gesture->copying)
    {
        selection.clear();

        for (const auto clip : gesture->clipsBeforeCopies)
            selection.add (clip);
    }

    // A Ctrl+click or a Shift+click whose pointer slipped still toggles the clip, as its click would. A plain press on a
    // selected clip that became a drag leaves the selection as it was: a drag by less than half a grid step moves nothing,
    // and isn't a click that selects the clip alone.
    if (gesture->waitedForClick && (mods.isCommandDown() || mods.isShiftDown()) && getClip (context, gesture->pressed).has_value())
    {
        selection.clear();

        for (const auto track : gesture->tracksBefore)
            selection.add (track);

        for (const auto clip : gesture->clipsBefore)
            selection.add (clip);

        for (const auto section : gesture->sectionsBefore)
            selection.add (section);

        clipClicked (gesture->pressed, mods);
    }
}

bool Playlist::cancelDrag()
{
    // Escape: what the press and its drag did goes, leaving nothing to undo or redo, what was selected and the view as they
    // were when the mouse went down; the mouse does nothing more until it goes up. In a lane, or the tempo lane, theirs.
    if (tempoLane.cancelDrag() || sectionsStrip.cancelDrag() || chordLane.cancelDrag()) // theirs too
        return true;

    for (auto* laneView : laneViews)
        if (laneView->cancelDrag())
            return true;

    if (gesture == nullptr)
        return false;

    gesture->rollBack();

    if (gesture->kind == Gesture::Kind::pan)
    {
        setViewStart (gesture->panViewStart);
        setScrollY (gesture->startScrollY);
    }
    else
    {
        gesture->selectionAtPress.putBack (context);
    }

    gesture = nullptr;
    setDragReadout ({});
    rubberBand = {};
    lanes->repaint();
    return true;
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
    else if (gesture->kind == Gesture::Kind::move && gesture->moved == BeatDuration() && gesture->rowOffset == 0)
    {
        endMoveThatWentNowhere (e.mods);
    }
    else if (gesture->kind == Gesture::Kind::move && gesture->rowOffset == 0)
    {
        // Clips moved or copied along their tracks take the automation under them, once, in the drag's undo step; clips
        // moved to other tracks leave it, as their devices are others.
        std::vector<ClipPlace> places;

        for (const auto& moving : gesture->moving)
            places.push_back (moving.place);

        moveAutomationWithClips (context, places, gesture->moved, gesture->copying);
    }

    gesture = nullptr;
    setDragReadout ({});

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

    if (! context.hasProject() || getTool() != Tool::smart || e.mods.isMiddleButtonDown()) // the middle button only moves the view
        return;

    // Under the last track: a new one at the end, as in Logic, Cubase and REAPER.
    if (row < 0)
    {
        if (e.y >= pinnedHeight + contentHeight - scrollY)
            addTrackAtEnd (false);

        return;
    }

    // On a clip: open it in its editor, the piano roll or the audio clip editor.
    if (const auto* hit = getClipAt (e.getPosition()))
    {
        if (onOpenClip != nullptr)
            onOpenClip (hit->id);

        return;
    }

    // On a track with an instrument: an empty MIDI clip, a bar long, from the grid line before the click.
    const auto track = getTrackRow (context, rows[(size_t) row].track.id);

    if (! track.has_value() || track->kind != TrackType::track || ! track->hasInstrument)
        return;

    addMidiClipAt (context, track->id, snapDown (getView().xToBeat (e.position.x), e.mods));
}
} // namespace phazite::ui
