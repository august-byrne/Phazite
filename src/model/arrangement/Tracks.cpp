// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Tracks.h"
#include "model/notes/Notes.h"

#include <algorithm>

namespace phazite
{
namespace
{
    const juce::Identifier heightProperty { "phaziteHeight" }, collapsedProperty { "phaziteCollapsed" }, pinnedProperty { "phazitePinned" };

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

    void selectNeighbour (te::SelectionManager& selection, int direction)
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

    // Moves the selected tracks one place up or down within their folders. A track swaps places with its
    // neighbour, so selected neighbours move together.
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

    // Mutes (or solos) the selected tracks, or unmutes them if they all are muted already.
    void toggleMuteOrSolo (te::SelectionManager& selection, bool mute)
    {
        const auto tracks = getSelectedTracks (selection);
        const auto isOn = [mute] (te::Track* track) { return mute ? track->isMuted (false) : track->isSolo (false); };
        const auto turnOn = ! std::all_of (tracks.begin(), tracks.end(), isOn);

        for (auto* track : tracks)
        {
            if (mute)
                track->setMute (turnOn);
            else
                track->setSolo (turnOn);
        }
    }

    bool areAllSelectedOn (const te::SelectionManager& selection, bool mute)
    {
        const auto tracks = getSelectedTracks (selection);
        return ! tracks.isEmpty()
               && std::all_of (tracks.begin(), tracks.end(),
                               [mute] (te::Track* track) { return mute ? track->isMuted (false) : track->isSolo (false); });
    }

    // Makes the selected tracks, or all tracks if none is selected, taller or shorter.
    void resizeTracks (te::SelectionManager& selection, int change)
    {
        auto tracks = getSelectedTracks (selection);

        if (tracks.isEmpty())
            if (auto* edit = selection.getEdit())
                tracks = getPlaylistTracks (*edit);

        for (auto* track : tracks)
            resizeTrack (*track, change);
    }
} // namespace

//==============================================================================
const std::array<TrackColour, 8>& getTrackColours()
{
    static const std::array<TrackColour, 8> colours { { { juce::Colour (0xffe69f00), "Orange" },
                                                        { juce::Colour (0xff56b4e9), "Sky blue" },
                                                        { juce::Colour (0xff009e73), "Bluish green" },
                                                        { juce::Colour (0xfff0e442), "Yellow" },
                                                        { juce::Colour (0xff0072b2), "Blue" },
                                                        { juce::Colour (0xffd55e00), "Vermilion" },
                                                        { juce::Colour (0xffcc79a7), "Reddish purple" },
                                                        { juce::Colour (0xff999999), "Gray" } } };
    return colours;
}

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
int getTrackHeight (const te::Track& track)
{
    return juce::jlimit (TrackHeight::minimum, TrackHeight::maximum, (int) track.state.getProperty (heightProperty, TrackHeight::standard));
}

void setTrackHeight (te::Track& track, int height)
{
    track.state.setProperty (heightProperty, juce::jlimit (TrackHeight::minimum, TrackHeight::maximum, height), nullptr);
}

int getShownTrackHeight (const te::Track& track)
{
    return isCollapsed (track) && ! track.isFolderTrack() ? TrackHeight::collapsed : getTrackHeight (track);
}

void setShownTrackHeight (te::Track& track, int height)
{
    if (track.isFolderTrack())
    {
        setTrackHeight (track, height);
        return;
    }

    // Halfway between the collapsed height and the minimum, the track snaps to one or the other.
    const auto collapse = height < (TrackHeight::collapsed + TrackHeight::minimum) / 2;

    if (! collapse)
        setTrackHeight (track, height);

    setCollapsed (track, collapse);
}

void resizeTrack (te::Track& track, int change)
{
    setShownTrackHeight (track, getShownTrackHeight (track) + change);
}

bool isCollapsed (const te::Track& track)
{
    return track.state.getProperty (collapsedProperty, false);
}

void setCollapsed (te::Track& track, bool collapsed)
{
    track.state.setProperty (collapsedProperty, collapsed, nullptr);
}

bool isPinned (const te::Track& track)
{
    return track.state.getProperty (pinnedProperty, false);
}

void setPinned (te::Track& track, bool pinned)
{
    track.state.setProperty (pinnedProperty, pinned, nullptr);
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
void addTrackCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto getEdit = [&selection] { return selection.getEdit(); };
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };
    const auto hasSelectedTracks = [&selection] { return ! getSelectedTracks (selection).isEmpty(); };

    registry.add ({ .id = "track.add",
                    .name = "Add Track",
                    .category = "Track",
                    .description = "Adds an empty track below the selected one, or at the bottom.",
                    .synonyms = { "new track", "insert track", "create" },
                    .perform =
                        [&selection]
                    {
                        if (auto* edit = selection.getEdit())
                            insertTrack (*edit, getSelectedTracks (selection).getLast(), &selection);
                    },
                    .isEnabled = hasEdit,
                    .undoTarget = getEdit });

    // Folders.
    const auto getSelectedFolders = [&selection]
    {
        juce::Array<te::FolderTrack*> folders;

        for (auto* track : getSelectedTracks (selection))
            if (auto* folder = dynamic_cast<te::FolderTrack*> (track))
                folders.add (folder);

        return folders;
    };

    const auto hasSelectedFolders = [getSelectedFolders] { return ! getSelectedFolders().isEmpty(); };

    registry.add ({ .id = "track.addFolder",
                    .name = "Add Folder",
                    .category = "Track",
                    .description = "Adds an empty folder below the selected track, or at the bottom.",
                    .synonyms = { "new folder", "group", "bus" },
                    .perform =
                        [&selection]
                    {
                        if (auto* edit = selection.getEdit())
                            insertFolder (*edit, getSelectedTracks (selection).getLast(), &selection);
                    },
                    .isEnabled = hasEdit,
                    .undoTarget = getEdit });

    registry.add ({ .id = "track.groupIntoFolder",
                    .name = "Group into Folder",
                    .category = "Track",
                    .description = "Puts the selected tracks into a new folder. A folder becomes a bus when it holds an effect.",
                    .synonyms = { "group", "folder", "bus", "submix" },
                    .perform = [&selection] { groupIntoFolder (getSelectedOutermostTracks (selection), &selection); },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit });

    registry.add ({ .id = "track.ungroup",
                    .name = "Ungroup Folder",
                    .category = "Track",
                    .description = "Takes the tracks out of the selected folders, and deletes the folders.",
                    .synonyms = { "remove folder", "ungroup", "unfolder" },
                    .perform =
                        [getSelectedFolders]
                    {
                        for (auto* folder : getSelectedFolders())
                            ungroupFolder (*folder);
                    },
                    .isEnabled = hasSelectedFolders,
                    .undoTarget = getEdit });

    // Collapsing and pinning change the view, so they aren't undo steps.
    registry.add ({ .id = "track.collapse",
                    .name = "Collapse Track",
                    .category = "Track",
                    .description = "Makes the selected tracks thin rows that show only their names, or full height again. A "
                                   "collapsed folder hides its tracks.",
                    .synonyms = { "fold", "unfold", "expand", "hide", "minimize", "folder" },
                    .perform =
                        [&selection]
                    {
                        const auto tracks = getSelectedTracks (selection);
                        const auto collapse = ! std::all_of (tracks.begin(), tracks.end(), [] (te::Track* t) { return isCollapsed (*t); });

                        for (auto* track : tracks)
                            setCollapsed (*track, collapse);
                    },
                    .isEnabled = hasSelectedTracks,
                    .isTicked =
                        [&selection]
                    {
                        const auto tracks = getSelectedTracks (selection);
                        return ! tracks.isEmpty()
                               && std::all_of (tracks.begin(), tracks.end(), [] (te::Track* t) { return isCollapsed (*t); });
                    } });

    registry.add ({ .id = "track.pin",
                    .name = "Pin Track",
                    .category = "Track",
                    .description = "Keeps the selected tracks in view at the top of the playlist while the others scroll.",
                    .synonyms = { "stick", "keep visible", "unpin" },
                    .perform =
                        [&selection]
                    {
                        const auto tracks = getSelectedTracks (selection);
                        const auto pin = ! std::all_of (tracks.begin(), tracks.end(), [] (te::Track* t) { return isPinned (*t); });

                        for (auto* track : tracks)
                            setPinned (*track, pin);
                    },
                    .isEnabled = hasSelectedTracks,
                    .isTicked =
                        [&selection]
                    {
                        const auto tracks = getSelectedTracks (selection);
                        return ! tracks.isEmpty()
                               && std::all_of (tracks.begin(), tracks.end(), [] (te::Track* t) { return isPinned (*t); });
                    } });

    registry.add ({ .id = "track.selectPrevious",
                    .name = "Select Track Above",
                    .category = "Track",
                    .description = "Selects the track above the selected one.",
                    .synonyms = { "previous track", "up" },
                    .perform = [&selection] { selectNeighbour (selection, -1); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.selectNext",
                    .name = "Select Track Below",
                    .category = "Track",
                    .description = "Selects the track below the selected one.",
                    .synonyms = { "next track", "down" },
                    .perform = [&selection] { selectNeighbour (selection, 1); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.moveUp",
                    .name = "Move Track Up",
                    .category = "Track",
                    .description = "Moves the selected tracks up one place.",
                    .synonyms = { "reorder", "raise" },
                    .perform = [&selection] { moveSelectedTracks (selection, -1); },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.moveDown",
                    .name = "Move Track Down",
                    .category = "Track",
                    .description = "Moves the selected tracks down one place.",
                    .synonyms = { "reorder", "lower" },
                    .perform = [&selection] { moveSelectedTracks (selection, 1); },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit,
                    .repeatsWhileHeld = true });

    // Tracktion keeps mute and solo out of the undo history, as many DAWs do.
    registry.add ({ .id = "track.mute",
                    .name = "Mute Track",
                    .category = "Track",
                    .description = "Silences the selected tracks, or unmutes them.",
                    .synonyms = { "silence", "unmute" },
                    .perform = [&selection] { toggleMuteOrSolo (selection, true); },
                    .isEnabled = hasSelectedTracks,
                    .isTicked = [&selection] { return areAllSelectedOn (selection, true); } });

    registry.add ({ .id = "track.solo",
                    .name = "Solo Track",
                    .category = "Track",
                    .description = "Plays only the selected tracks and any others that are soloed, or ends their solo.",
                    .synonyms = { "isolate", "unsolo" },
                    .perform = [&selection] { toggleMuteOrSolo (selection, false); },
                    .isEnabled = hasSelectedTracks,
                    .isTicked = [&selection] { return areAllSelectedOn (selection, false); } });

    registry.add ({ .id = "track.taller",
                    .name = "Taller Tracks",
                    .category = "Track",
                    .description = "Makes the selected tracks taller, or all tracks if none is selected.",
                    .synonyms = { "height", "bigger", "vertical zoom", "expand" },
                    .perform = [&selection] { resizeTracks (selection, TrackHeight::step); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add (
        { .id = "track.shorter",
          .name = "Shorter Tracks",
          .category = "Track",
          .description =
              "Makes the selected tracks shorter, or all tracks if none is selected. Past the shortest, a track collapses to its name.",
          .synonyms = { "height", "smaller", "vertical zoom", "compact" },
          .perform = [&selection] { resizeTracks (selection, -TrackHeight::step); },
          .isEnabled = hasEdit,
          .repeatsWhileHeld = true });
}
} // namespace phazite
