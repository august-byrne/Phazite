// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/IDsHeld.h"
#include "model/base/Song.h"
#include "model/base/internal/ClipsArea.h"
#include "model/base/internal/NotesArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsArea.h"
#include "model/base/internal/SongValues.h"

#include <algorithm>

namespace phazite
{
std::uint64_t findLargestIDHeld (const Song& song)
{
    const auto* values = song.getValues();

    if (values == nullptr)
        return 0;

    // Each area that holds IDs, a line each: the clips' (each clip's ID and its alias group's), the notes' (each note's ID).
    std::uint64_t largest = 0;
    largest = std::max (largest, findLargestIDIn (values->clips.get()));
    largest = std::max (largest, findLargestIDIn (values->notes.get()));
    largest = std::max (largest, findLargestIDIn (values->sections.get())); // the sections' (each section's ID)
    return largest;
}

std::uint64_t countHeldID (std::uint64_t largest, std::uint64_t held) noexcept
{
    return held < schema::maxID ? std::max (largest, held) : largest;
}
} // namespace phazite
