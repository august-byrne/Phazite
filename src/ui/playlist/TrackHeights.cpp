// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackHeights.h"
#include "model/commands/CommandRegistry.h"

namespace phazite::ui
{
namespace
{
    // The selected tracks, or all tracks if none is selected.
    juce::Array<te::Track*> getTracksToResize (const te::SelectionManager& selection)
    {
        auto tracks = getSelectedTracks (selection);

        if (tracks.isEmpty())
            if (auto* edit = selection.getEdit())
                tracks = getPlaylistTracks (*edit);

        return tracks;
    }
} // namespace

int getTrackHeight (const te::Track& track)
{
    return juce::jlimit (TrackHeight::minimum, TrackHeight::maximum, getSavedTrackHeight (track).value_or (TrackHeight::standard));
}

void setTrackHeight (te::Track& track, int height)
{
    setSavedTrackHeight (track, juce::jlimit (TrackHeight::minimum, TrackHeight::maximum, height));
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

void resizeTracks (const juce::Array<te::Track*>& tracks, int change)
{
    for (auto* track : tracks)
        resizeTrack (*track, change);
}

void addTrackViewCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "track.taller",
                    .name = "Taller Tracks",
                    .category = "Track",
                    .description = "Makes the selected tracks taller, or all tracks if none is selected.",
                    .synonyms = { "height", "bigger", "vertical zoom", "expand" },
                    .perform = [&selection] { resizeTracks (getTracksToResize (selection), TrackHeight::step); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add (
        { .id = "track.shorter",
          .name = "Shorter Tracks",
          .category = "Track",
          .description =
              "Makes the selected tracks shorter, or all tracks if none is selected. Past the shortest, a track collapses to its name.",
          .synonyms = { "height", "smaller", "vertical zoom", "compact" },
          .perform = [&selection] { resizeTracks (getTracksToResize (selection), -TrackHeight::step); },
          .isEnabled = hasEdit,
          .repeatsWhileHeld = true });
}
} // namespace phazite::ui
