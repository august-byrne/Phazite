// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Tracks.h"

#include <algorithm>

namespace phazite
{
namespace
{
    const juce::Identifier heightProperty { "phaziteHeight" };

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

    // The selected tracks, without those in a selected folder: they go wherever their folder goes.
    juce::Array<te::Track*> getSelectedOutermostTracks (const te::SelectionManager& selection)
    {
        juce::Array<te::Track*> tracks;

        for (auto* track : getSelectedTracks (selection))
        {
            auto inSelectedFolder = false;

            for (auto* folder = track->getParentFolderTrack(); folder != nullptr && ! inSelectedFolder;
                 folder = folder->getParentFolderTrack())
                inSelectedFolder = selection.isSelected (folder);

            if (! inSelectedFolder)
                tracks.add (track);
        }

        return tracks;
    }

    // "Delete Track", "Delete 3 Tracks", or just "Delete" with no track selected.
    juce::String nameForSelection (const te::SelectionManager& selection, const juce::String& verb)
    {
        const auto count = getSelectedTracks (selection).size();

        if (count == 0)
            return verb;

        return verb + (count == 1 ? juce::String (" Track") : " " + juce::String (count) + " Tracks");
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
            setTrackHeight (*track, getTrackHeight (*track) + change);
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

//==============================================================================
te::AudioTrack* insertTrack (te::Edit& edit, te::Track* after, te::SelectionManager* selection)
{
    const auto tracks = getPlaylistTracks (edit);
    auto track =
        edit.insertNewAudioTrack (after != nullptr ? getInsertPointAfter (*after) : te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

    if (track == nullptr)
        return nullptr;

    // The palette's next color after the track above, so neighbours differ.
    const auto& colours = getTrackColours();
    size_t next = 0;

    if (auto* above = after != nullptr ? after : tracks.getLast())
    {
        const auto aboveColour = getTrackColour (*above, tracks.indexOf (above));

        for (size_t i = 0; i < colours.size(); ++i)
            if (colours[i].colour == aboveColour)
                next = (i + 1) % colours.size();
    }

    track->setColour (colours[next].colour);

    if (selection != nullptr)
        selection->selectOnly (track.get());

    return track.get();
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

    registry.add ({ .id = "edit.delete",
                    .name = "Delete",
                    .category = "Edit",
                    .description = "Deletes the selection, such as the selected tracks.",
                    .synonyms = { "remove", "erase" },
                    .perform =
                        [&selection]
                    {
                        auto* edit = selection.getEdit();
                        const auto tracks = getSelectedOutermostTracks (selection);

                        if (edit == nullptr || tracks.isEmpty())
                            return;

                        const auto row = getPlaylistTracks (*edit).indexOf (tracks.getFirst());

                        for (auto* track : tracks)
                            edit->deleteTrack (track);

                        // The track that takes the first one's place is selected, as in Ableton Live, so
                        // the keyboard can carry on from there.
                        if (const auto remaining = getPlaylistTracks (*edit); ! remaining.isEmpty())
                            selection.selectOnly (remaining[juce::jmin (row, remaining.size() - 1)]);
                    },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit,
                    .currentName = [&selection] { return nameForSelection (selection, "Delete"); } });

    registry.add ({ .id = "edit.duplicate",
                    .name = "Duplicate",
                    .category = "Edit",
                    .description = "Duplicates the selection, such as the selected tracks: each copy goes right after its original.",
                    .synonyms = { "copy", "clone", "repeat" },
                    .perform =
                        [&selection]
                    {
                        auto* edit = selection.getEdit();

                        if (edit == nullptr)
                            return;

                        juce::Array<te::Track*> copies;

                        for (auto* track : getSelectedOutermostTracks (selection))
                            if (auto copy = edit->copyTrack (track, getInsertPointAfter (*track)))
                                copies.add (copy.get());

                        selection.deselectAll();

                        for (auto* copy : copies)
                            selection.addToSelection (copy);
                    },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit,
                    .currentName = [&selection] { return nameForSelection (selection, "Duplicate"); } });

    registry.add ({ .id = "edit.selectAll",
                    .name = "Select All",
                    .category = "Edit",
                    .description = "Selects every track.",
                    .synonyms = { "everything" },
                    .perform =
                        [&selection]
                    {
                        if (auto* edit = selection.getEdit())
                        {
                            selection.deselectAll();

                            for (auto* track : getPlaylistTracks (*edit))
                                selection.addToSelection (track);
                        }
                    },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "edit.deselectAll",
                    .name = "Deselect All",
                    .category = "Edit",
                    .description = "Clears the selection.",
                    .synonyms = { "select none", "clear selection" },
                    .perform = [&selection] { selection.deselectAll(); },
                    .isEnabled = [&selection] { return selection.getNumObjectsSelected() > 0; } });

    registry.add ({ .id = "track.selectPrevious",
                    .name = "Select Track Above",
                    .category = "Track",
                    .description = "Selects the track above the selected one.",
                    .synonyms = { "previous track", "up" },
                    .perform = [&selection] { selectNeighbour (selection, -1); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "track.selectNext",
                    .name = "Select Track Below",
                    .category = "Track",
                    .description = "Selects the track below the selected one.",
                    .synonyms = { "next track", "down" },
                    .perform = [&selection] { selectNeighbour (selection, 1); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "track.moveUp",
                    .name = "Move Track Up",
                    .category = "Track",
                    .description = "Moves the selected tracks up one place.",
                    .synonyms = { "reorder", "raise" },
                    .perform = [&selection] { moveSelectedTracks (selection, -1); },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit });

    registry.add ({ .id = "track.moveDown",
                    .name = "Move Track Down",
                    .category = "Track",
                    .description = "Moves the selected tracks down one place.",
                    .synonyms = { "reorder", "lower" },
                    .perform = [&selection] { moveSelectedTracks (selection, 1); },
                    .isEnabled = hasSelectedTracks,
                    .undoTarget = getEdit });

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
                    .isEnabled = hasEdit });

    registry.add ({ .id = "track.shorter",
                    .name = "Shorter Tracks",
                    .category = "Track",
                    .description = "Makes the selected tracks shorter, or all tracks if none is selected.",
                    .synonyms = { "height", "smaller", "vertical zoom", "compact" },
                    .perform = [&selection] { resizeTracks (selection, -TrackHeight::step); },
                    .isEnabled = hasEdit });
}
} // namespace phazite
