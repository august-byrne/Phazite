// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/IDsHeld.h"
#include "model/base/Song.h"
#include "model/base/internal/ClipsArea.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/NotesArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsArea.h"
#include "model/base/internal/SongValues.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Whether the song holds its modulators and macros beside its tree: not yet (modulation's first piece, roadmap step
    // S5.20: the tree is their storage). forEachNameableIDHeld()'s line for them reads nothing until it is on.
    constexpr bool modulationHeldBesideTheTree = false;
} // namespace

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

void forEachNameableIDHeld (const Song& song, const std::function<void (std::uint64_t)>& each)
{
    // Each area whose value holds items that something can name, a line each, in no order that matters. Modulation's (each
    // modulator's and macro's: ModulationValue.h) only once the song holds it beside its tree: until then the tree holds
    // them, which the caller's walk of the tree finds (Schema.h: holdsID() of a MODULATOR and a MACRO), and reading them
    // again here would cost every removal a read of the song's whole modulation. Modulation's switch (its third piece)
    // turns modulationHeldBesideTheTree on and reads the area's value in place of the tree's.
    if constexpr (modulationHeldBesideTheTree)
        forEachIDHeld (readModulationValue (song.getState()), each);
    else
        juce::ignoreUnused (song, each);
}

std::uint64_t countHeldID (std::uint64_t largest, std::uint64_t held) noexcept
{
    return held < schema::maxID ? std::max (largest, held) : largest;
}
} // namespace phazite
