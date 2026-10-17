// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // The palette's next color after the track a new one goes below (the last track if that is nullptr),
    // so that neighbours differ.
    juce::Colour getNextColour (te::Edit& edit, te::Track* after)
    {
        const auto tracks = getPlaylistTracks (edit);
        const auto& colours = getTrackColours();
        size_t next = 0;

        if (auto* above = after != nullptr ? after : tracks.getLast())
        {
            const auto aboveColour = getTrackColour (*above, tracks.indexOf (above));

            for (size_t i = 0; i < colours.size(); ++i)
                if (colours[i].colour == aboveColour)
                    next = (i + 1) % colours.size();
        }

        return colours[next].colour;
    }

    bool isPlaylistTrack (const te::Track& track)
    {
        return track.isAudioTrack() || track.isFolderTrack();
    }

    // The playlist track before (direction -1) or after (1) this one in its folder, or among the
    // top-level tracks.
    te::Track* getSibling (te::Track& track, int direction)
    {
        const auto parent = track.state.getParent();

        for (auto i = parent.indexOf (track.state) + direction; juce::isPositiveAndBelow (i, parent.getNumChildren()); i += direction)
            if (auto* sibling = te::findTrackForState (track.edit, parent.getChild (i)); sibling != nullptr && isPlaylistTrack (*sibling))
                return sibling;

        return nullptr;
    }
} // namespace

//==============================================================================
juce::Colour getTrackColour (const te::Track& track, int row)
{
    if (const auto colour = track.getColour(); ! colour.isTransparent())
        return colour;

    const auto& colours = getTrackColours();
    return colours[(size_t) (std::max (0, row) % (int) colours.size())].colour;
}

//==============================================================================
juce::Array<te::Track*> getPlaylistTracks (const te::Edit& edit)
{
    juce::Array<te::Track*> tracks;

    edit.visitAllTracks (
        [&tracks] (te::Track& track)
        {
            if (isPlaylistTrack (track))
                tracks.add (&track);

            return true;
        },
        true);

    return tracks;
}

juce::Array<te::Track*> getSelectedTracks (const te::SelectionManager& selection)
{
    juce::Array<te::Track*> selected;

    if (auto* edit = selection.getEdit())
        for (auto* track : getPlaylistTracks (*edit))
            if (selection.isSelected (track))
                selected.add (track);

    return selected;
}

juce::Array<te::Track*> getSelectedOutermostTracks (const te::SelectionManager& selection)
{
    juce::Array<te::Track*> tracks;

    for (auto* track : getSelectedTracks (selection))
    {
        auto inSelectedFolder = false;

        for (auto* folder = track->getParentFolderTrack(); folder != nullptr && ! inSelectedFolder; folder = folder->getParentFolderTrack())
            inSelectedFolder = selection.isSelected (folder);

        if (! inSelectedFolder)
            tracks.add (track);
    }

    return tracks;
}

te::TrackInsertPoint getInsertPointAfter (te::Track& track)
{
    return { track.getParentTrack(), &track };
}

//==============================================================================
std::optional<int> getSavedTrackHeight (const te::Track& track)
{
    if (const auto* height = track.state.getPropertyPointer (schema::phaziteHeight))
        return (int) *height;

    return std::nullopt;
}

void setSavedTrackHeight (te::Track& track, int height)
{
    track.state.setProperty (schema::phaziteHeight, height, nullptr);
}

bool isCollapsed (const te::Track& track)
{
    return track.state.getProperty (schema::phaziteCollapsed, false);
}

void setCollapsed (te::Track& track, bool collapsed)
{
    track.state.setProperty (schema::phaziteCollapsed, collapsed, nullptr);
}

bool isPinned (const te::Track& track)
{
    return track.state.getProperty (schema::phazitePinned, false);
}

void setPinned (te::Track& track, bool pinned)
{
    track.state.setProperty (schema::phazitePinned, pinned, nullptr);
}

bool isHiddenInCollapsedFolder (const te::Track& track)
{
    for (auto* folder = track.getParentFolderTrack(); folder != nullptr; folder = folder->getParentFolderTrack())
        if (isCollapsed (*folder))
            return true;

    return false;
}

void toggleSoloAlone (te::Track& track)
{
    auto alone = track.isSolo (false);

    for (auto* other : te::getAllTracks (track.edit))
        if (other != &track && other->isSolo (false))
            alone = false;

    for (auto* other : te::getAllTracks (track.edit))
        if (other != &track)
            other->setSolo (false);

    track.setSolo (! alone);
}

bool isSilencedBySolo (const te::Track& track)
{
    return track.edit.areAnyTracksSolo() && ! track.isSolo (true) && ! track.isSoloIsolate (true) && ! track.isMuted (false);
}

//==============================================================================
te::AudioTrack* insertTrack (te::Edit& edit, te::Track* after, te::SelectionManager* selection)
{
    const auto colour = getNextColour (edit, after);
    auto track =
        edit.insertNewAudioTrack (after != nullptr ? getInsertPointAfter (*after) : te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

    if (track == nullptr)
        return nullptr;

    track->setColour (colour);
    applyLastKey (*track); // the key chosen last in the song

    if (selection != nullptr)
        selection->selectOnly (track.get());

    return track.get();
}

te::FolderTrack* insertFolder (te::Edit& edit, te::Track* after, te::SelectionManager* selection)
{
    const auto colour = getNextColour (edit, after);
    auto folder = edit.insertNewFolderTrack (after != nullptr ? getInsertPointAfter (*after) : te::TrackInsertPoint::getEndOfTracks (edit),
                                             nullptr, false);

    if (folder == nullptr)
        return nullptr;

    folder->setName ("Folder");
    mixAsBus (*folder); // a bus from the start (design question 11)
    folder->setColour (colour);

    if (selection != nullptr)
        selection->selectOnly (folder.get());

    return folder.get();
}

te::FolderTrack* groupIntoFolder (const juce::Array<te::Track*>& tracks, te::SelectionManager* selection)
{
    if (tracks.isEmpty())
        return nullptr;

    // Where the first track is, in its own folder if it has one; in its color.
    auto& first = *tracks.getFirst();
    const auto colour = getTrackColour (first, getPlaylistTracks (first.edit).indexOf (&first));
    auto folder = first.edit.insertNewFolderTrack (te::TrackInsertPoint (first, true), nullptr, false);

    if (folder == nullptr)
        return nullptr;

    folder->setName ("Folder");
    mixAsBus (*folder); // a bus from the start (design question 11)
    folder->setColour (colour);

    for (auto* track : tracks)
        moveTrackIntoFolder (*track, *folder);

    if (selection != nullptr)
        selection->selectOnly (folder.get());

    return folder.get();
}

void moveTrackIntoFolder (te::Track& track, te::FolderTrack& folder)
{
    // A folder can't go into itself, or into a folder inside it.
    if (&track == &folder || folder.isAChildOf (track))
        return;

    const auto children = folder.getAllSubTracks (false);
    track.edit.moveTrack (&track, children.isEmpty() ? te::TrackInsertPoint (&folder, nullptr) : getInsertPointAfter (*children.getLast()));
}

void ungroupFolder (te::FolderTrack& folder)
{
    auto& edit = folder.edit;
    te::Track* previous = &folder;

    for (auto* child : folder.getAllSubTracks (false))
    {
        edit.moveTrack (child, getInsertPointAfter (*previous));
        previous = child;
    }

    edit.deleteTrack (&folder);
}

//==============================================================================
bool hasSelectedTracks (const te::SelectionManager& selection)
{
    return ! getSelectedTracks (selection).isEmpty();
}

juce::Array<te::FolderTrack*> getSelectedFolders (const te::SelectionManager& selection)
{
    juce::Array<te::FolderTrack*> folders;

    for (auto* track : getSelectedTracks (selection))
        if (auto* folder = dynamic_cast<te::FolderTrack*> (track))
            folders.add (folder);

    return folders;
}

bool hasSelectedFolders (const te::SelectionManager& selection)
{
    return ! getSelectedFolders (selection).isEmpty();
}

juce::Array<te::Track*> getSelectedTracksOrAll (const te::SelectionManager& selection)
{
    auto tracks = getSelectedTracks (selection);

    if (tracks.isEmpty())
        if (auto* edit = selection.getEdit())
            tracks = getPlaylistTracks (*edit);

    return tracks;
}

te::AudioTrack* insertTrackAfterSelection (te::SelectionManager& selection)
{
    auto* edit = selection.getEdit();
    return edit != nullptr ? insertTrack (*edit, getSelectedTracks (selection).getLast(), &selection) : nullptr;
}

te::FolderTrack* insertFolderAfterSelection (te::SelectionManager& selection)
{
    auto* edit = selection.getEdit();
    return edit != nullptr ? insertFolder (*edit, getSelectedTracks (selection).getLast(), &selection) : nullptr;
}

void ungroupFolders (const juce::Array<te::FolderTrack*>& folders)
{
    for (auto* folder : folders)
        ungroupFolder (*folder);
}

void selectNeighbourTrack (te::SelectionManager& selection, int direction)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return;

    const auto tracks = getPlaylistTracks (*edit);
    const auto selected = getSelectedTracks (selection);

    if (tracks.isEmpty())
        return;

    if (selected.isEmpty())
    {
        selection.selectOnly (direction > 0 ? tracks.getFirst() : tracks.getLast());
        return;
    }

    const auto row = tracks.indexOf (direction > 0 ? selected.getLast() : selected.getFirst()) + direction;
    selection.selectOnly (tracks[juce::jlimit (0, tracks.size() - 1, row)]);
}

void moveSelectedTracks (te::SelectionManager& selection, int direction)
{
    auto* edit = selection.getEdit();
    auto tracks = getSelectedOutermostTracks (selection);

    if (edit == nullptr)
        return;

    // The track nearest to where they move goes first.
    if (direction > 0)
        std::reverse (tracks.begin(), tracks.end());

    for (auto* track : tracks)
    {
        auto* neighbour = getSibling (*track, direction);

        if (neighbour == nullptr || selection.isSelected (neighbour))
            continue;

        if (direction < 0)
            edit->moveTrack (neighbour, getInsertPointAfter (*track)); // the one above goes below
        else
            edit->moveTrack (track, getInsertPointAfter (*neighbour));
    }
}

bool areAllTracksMuted (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return track->isMuted (false); });
}

void toggleTracksMuted (const juce::Array<te::Track*>& tracks)
{
    toggleAll (tracks, [] (te::Track* track) { return track->isMuted (false); }, [] (te::Track* track, bool on) { track->setMute (on); });
}

bool areAllTracksSoloed (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return track->isSolo (false); });
}

void toggleTracksSoloed (const juce::Array<te::Track*>& tracks)
{
    toggleAll (tracks, [] (te::Track* track) { return track->isSolo (false); }, [] (te::Track* track, bool on) { track->setSolo (on); });
}

bool areAllTracksCollapsed (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return isCollapsed (*track); });
}

void toggleTracksCollapsed (const juce::Array<te::Track*>& tracks)
{
    toggleAll (
        tracks, [] (te::Track* track) { return isCollapsed (*track); }, [] (te::Track* track, bool on) { setCollapsed (*track, on); });
}

bool areAllTracksPinned (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return isPinned (*track); });
}

void toggleTracksPinned (const juce::Array<te::Track*>& tracks)
{
    toggleAll (tracks, [] (te::Track* track) { return isPinned (*track); }, [] (te::Track* track, bool on) { setPinned (*track, on); });
}

juce::Array<te::Track*> duplicateTracks (const juce::Array<te::Track*>& tracks, te::SelectionManager* selection)
{
    juce::Array<te::Track*> copies;

    for (auto* track : tracks)
        if (auto copy = track->edit.copyTrack (track, getInsertPointAfter (*track)))
        {
            copyLanes (*track, *copy); // its lanes show its own devices
            copies.add (copy.get());
        }

    if (selection != nullptr)
    {
        selection->deselectAll();

        for (auto* copy : copies)
            selection->addToSelection (copy);
    }

    return copies;
}

void renameTrack (te::Track& track, const juce::String& name)
{
    const auto trimmed = name.trim();

    if (trimmed == track.getName())
        return;

    const UndoStep step (track.edit, "Rename Track");
    track.setName (trimmed);
}

te::Track* addTrackAtEnd (te::Edit& edit, bool folder, te::SelectionManager* selection)
{
    const UndoStep step (edit, folder ? "Add Folder" : "Add Track");

    if (folder)
        return insertFolder (edit, nullptr, selection);

    return insertTrack (edit, nullptr, selection);
}

void setTrackColours (const juce::Array<te::Track*>& tracks, juce::Colour colour)
{
    if (tracks.isEmpty())
        return;

    const UndoStep step (tracks.getFirst()->edit, tracks.size() > 1 ? "Change Track Colors" : "Change Track Color");

    for (auto* track : tracks)
        track->setColour (colour);
}

void moveTracksAfter (const juce::Array<te::Track*>& moving, te::Track* previous)
{
    if (moving.isEmpty())
        return;

    auto& edit = moving.getFirst()->edit;
    const UndoStep step (edit, moving.size() > 1 ? "Move Tracks" : "Move Track");

    // To the playlist's first place: after Tracktion's global tracks (tempo, markers...).
    te::Track* lastGlobalTrack = nullptr;

    edit.getTrackList().visitAllTopLevel (
        [&lastGlobalTrack] (te::Track& track)
        {
            if (track.isAudioTrack() || track.isFolderTrack())
                return false;

            lastGlobalTrack = &track;
            return true;
        });

    for (auto* track : moving)
    {
        edit.moveTrack (track, previous != nullptr ? getInsertPointAfter (*previous) : te::TrackInsertPoint (nullptr, lastGlobalTrack));
        previous = track;
    }
}

void deleteTracks (const juce::Array<te::Track*>& tracks, te::SelectionManager* selection)
{
    // A folder takes the tracks inside it along, so those aren't deleted on their own.
    juce::Array<te::Track*> outermost;

    for (auto* track : tracks)
    {
        auto inDeletedFolder = false;

        for (auto* folder = track->getParentFolderTrack(); folder != nullptr && ! inDeletedFolder; folder = folder->getParentFolderTrack())
            inDeletedFolder = tracks.contains (folder);

        if (! inDeletedFolder)
            outermost.add (track);
    }

    if (outermost.isEmpty())
        return;

    auto& edit = outermost.getFirst()->edit;
    const auto row = getPlaylistTracks (edit).indexOf (outermost.getFirst());
    forgetSidechainSources (outermost);

    for (auto* track : outermost)
        edit.deleteTrack (track);

    if (selection != nullptr)
        if (const auto remaining = getPlaylistTracks (edit); ! remaining.isEmpty())
            selection->selectOnly (remaining[juce::jmin (row, remaining.size() - 1)]);
}

//==============================================================================
void addTrackCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "track.add",
                    .name = "Add Track",
                    .category = "Track",
                    .description = "Adds an empty track below the selected one, or at the bottom.",
                    .synonyms = { "new track", "insert track", "create" },
                    .perform = [&selection] { insertTrackAfterSelection (selection); },
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget });

    // Folders.
    registry.add ({ .id = "track.addFolder",
                    .name = "Add Folder",
                    .category = "Track",
                    .description = "Adds an empty folder below the selected track, or at the bottom.",
                    .synonyms = { "new folder", "group", "bus" },
                    .perform = [&selection] { insertFolderAfterSelection (selection); },
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget });

    registry.add ({ .id = "track.groupIntoFolder",
                    .name = "Group into Folder",
                    .category = "Track",
                    .description = "Puts the selected tracks into a new folder. A folder becomes a bus when it holds an effect.",
                    .synonyms = { "group", "folder", "bus", "submix" },
                    .perform = [&selection] { groupIntoFolder (getSelectedOutermostTracks (selection), &selection); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "track.ungroup",
                    .name = "Ungroup Folder",
                    .category = "Track",
                    .description = "Takes the tracks out of the selected folders, and deletes the folders.",
                    .synonyms = { "remove folder", "ungroup", "unfolder" },
                    .perform = [&selection] { ungroupFolders (getSelectedFolders (selection)); },
                    .isEnabled = [&selection] { return hasSelectedFolders (selection); },
                    .undoTarget = undoTarget });

    // Collapsing and pinning change the view, so they aren't undo steps.
    registry.add ({ .id = "track.collapse",
                    .name = "Collapse Track",
                    .category = "Track",
                    .description = "Makes the selected tracks thin rows that show only their names, or full height again. A "
                                   "collapsed folder hides its tracks.",
                    .synonyms = { "fold", "unfold", "expand", "hide", "minimize", "folder" },
                    .perform = [&selection] { toggleTracksCollapsed (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksCollapsed (getSelectedTracks (selection)); } });

    registry.add ({ .id = "track.pin",
                    .name = "Pin Track",
                    .category = "Track",
                    .description = "Keeps the selected tracks in view at the top of the playlist while the others scroll.",
                    .synonyms = { "stick", "keep visible", "unpin" },
                    .perform = [&selection] { toggleTracksPinned (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksPinned (getSelectedTracks (selection)); } });

    registry.add ({ .id = "track.selectPrevious",
                    .name = "Select Track Above",
                    .category = "Track",
                    .description = "Selects the track above the selected one.",
                    .synonyms = { "previous track", "up" },
                    .perform = [&selection] { selectNeighbourTrack (selection, -1); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.selectNext",
                    .name = "Select Track Below",
                    .category = "Track",
                    .description = "Selects the track below the selected one.",
                    .synonyms = { "next track", "down" },
                    .perform = [&selection] { selectNeighbourTrack (selection, 1); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.moveUp",
                    .name = "Move Track Up",
                    .category = "Track",
                    .description = "Moves the selected tracks up one place.",
                    .synonyms = { "reorder", "raise" },
                    .perform = [&selection] { moveSelectedTracks (selection, -1); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.moveDown",
                    .name = "Move Track Down",
                    .category = "Track",
                    .description = "Moves the selected tracks down one place.",
                    .synonyms = { "reorder", "lower" },
                    .perform = [&selection] { moveSelectedTracks (selection, 1); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget,
                    .repeatsWhileHeld = true });

    // Tracktion keeps mute and solo out of the undo history, as many DAWs do.
    registry.add ({ .id = "track.mute",
                    .name = "Mute Track",
                    .category = "Track",
                    .description = "Silences the selected tracks, or unmutes them.",
                    .synonyms = { "silence", "unmute" },
                    .perform = [&selection] { toggleTracksMuted (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksMuted (getSelectedTracks (selection)); } });

    registry.add ({ .id = "track.solo",
                    .name = "Solo Track",
                    .category = "Track",
                    .description = "Plays only the selected tracks and any others that are soloed, or ends their solo.",
                    .synonyms = { "isolate", "unsolo" },
                    .perform = [&selection] { toggleTracksSoloed (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksSoloed (getSelectedTracks (selection)); } });
}
} // namespace phazite
