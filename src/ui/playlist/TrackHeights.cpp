// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackHeights.h"
#include "model/commands/CommandRegistry.h"
#include "model/project/ProjectContext.h"

namespace phazite::ui
{
namespace
{
    // The selected tracks, or all of the playlist's if none is selected.
    std::vector<TrackID> getSelectedTracksOrAll (const ProjectContext& context)
    {
        const auto selected = context.getSongSelection().getTracks();
        return selected.empty() ? getAllPlaylistTracks (context) : selected;
    }
} // namespace

int getTrackHeight (const TrackRow& row)
{
    return juce::jlimit (TrackHeight::minimum, TrackHeight::maximum, row.savedHeight.value_or (TrackHeight::standard));
}

int getShownTrackHeight (const TrackRow& row)
{
    return row.collapsed && row.kind != TrackType::folder ? TrackHeight::collapsed : getTrackHeight (row);
}

void setShownTrackHeight (ProjectContext& context, TrackID id, int height)
{
    const auto row = getTrackRow (context, id);

    if (! row.has_value())
        return;

    const auto kept = juce::jlimit (TrackHeight::minimum, TrackHeight::maximum, height);

    if (row->kind == TrackType::folder)
    {
        setSavedTrackHeight (context, id, kept);
        return;
    }

    // Halfway between the collapsed height and the minimum, the track snaps to one or the other.
    const auto collapse = height < (TrackHeight::collapsed + TrackHeight::minimum) / 2;

    if (! collapse)
        setSavedTrackHeight (context, id, kept);

    setCollapsed (context, id, collapse);
}

void resizeTracks (ProjectContext& context, const std::vector<TrackID>& ids, int change)
{
    for (const auto id : ids)
        if (const auto row = getTrackRow (context, id))
            setShownTrackHeight (context, id, getShownTrackHeight (*row) + change);
}

void addTrackViewCommands (CommandRegistry& registry, ProjectContext& context)
{
    const auto hasProject = [&context] { return context.hasProject(); };

    registry.add ({ .id = "track.taller",
                    .name = "Taller Tracks",
                    .category = "Track",
                    .description = "Makes the selected tracks taller, or all tracks if none is selected.",
                    .synonyms = { "height", "bigger", "vertical zoom", "expand" },
                    .perform = [&context] { resizeTracks (context, getSelectedTracksOrAll (context), TrackHeight::step); },
                    .isEnabled = hasProject,
                    .repeatsWhileHeld = true });

    registry.add (
        { .id = "track.shorter",
          .name = "Shorter Tracks",
          .category = "Track",
          .description =
              "Makes the selected tracks shorter, or all tracks if none is selected. Past the shortest, a track collapses to its name.",
          .synonyms = { "height", "smaller", "vertical zoom", "compact" },
          .perform = [&context] { resizeTracks (context, getSelectedTracksOrAll (context), -TrackHeight::step); },
          .isEnabled = hasProject,
          .repeatsWhileHeld = true });
}
} // namespace phazite::ui
