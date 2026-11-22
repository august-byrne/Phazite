// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/IDsHeld.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // The largest of the IDs it is given, below schema::maxID.
    struct Largest
    {
        std::uint64_t id = 0;

        void count (std::uint64_t held) noexcept
        {
            if (held < schema::maxID)
                id = std::max (id, held);
        }
    };

    // The clips' area: each clip's ID and its alias group's.
    void countClips (const SongClips& clips, Largest& largest)
    {
        for (const auto& entry : clips.clips)
        {
            largest.count (entry.second.id.getRaw());
            largest.count (entry.second.link);
        }
    }

    // The notes' area: each note's ID (an alias group's clips hold one value, counted under each of their IDs).
    void countNotes (const NotesByClip& notes, Largest& largest)
    {
        for (const auto& entry : notes.clips)
            for (const auto& note : entry.second.notes)
                largest.count (note.id.getRaw());
    }
} // namespace

std::uint64_t findLargestIDHeld (const Song& song)
{
    const auto* values = song.getValues();

    if (values == nullptr)
        return 0;

    Largest largest;
    countClips (values->clips.get(), largest);
    countNotes (values->notes.get(), largest);
    return largest.id;
}
} // namespace phazite
