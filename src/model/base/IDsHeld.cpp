// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/IDsHeld.h"
#include "model/base/Song.h"
#include "model/base/internal/ClipsArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/NotesArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsArea.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksValue.h"

#include <algorithm>
#include <set>

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
    largest = std::max (largest, findLargestIDIn (values->sections.get()));   // the sections' (each section's ID)
    largest = std::max (largest, findLargestIDIn (values->modulation.get())); // modulation's (each modulator's and macro's ID)
    largest = std::max (largest, findLargestIDIn (values->sends.get()));      // the sends' (each send's ID)
    largest = std::max (largest, findLargestIDIn (values->devices.get()));    // the devices' (a device's, container's, chain's)
    largest = std::max (largest, findLargestIDIn (values->tracks.get()));     // the tracks' (a track's, its fader's and VCA's)
    return largest;
}

void forEachNameableIDHeld (const Song& song, const std::function<void (std::uint64_t)>& each)
{
    // Each area whose value holds items that something can name, a line each, in no order that matters, each from its area's
    // value: modulation's (each modulator's and macro's: ModulationValue.h, what a MIDI link of a macro's value or of a
    // modulator's parameter and a macro's value's curve name), and the sends' (each send's ID: SendsValue.h, what a curve, a
    // MIDI link and a modulation name a send's level by).
    if (const auto* values = song.getValues())
    {
        forEachIDHeld (values->modulation.get(), each);
        forEachIDHeld (values->sends.get(), each);
        forEachIDHeld (values->devices.get(), each); // the devices' (each device's, container's and chain's: DevicesValue.h)

        // The tracks' (each track's, its fader's and its VCA's, and the master's: TracksValue.h), what a curve, a lane, a
        // modulation and a MIDI link name a fader by and a send, a sidechain and a modulator's place a track by.
        forEachIDHeld (values->tracks.get(), each);
    }
}

std::uint64_t countHeldID (std::uint64_t largest, std::uint64_t held) noexcept
{
    return held < schema::maxID ? std::max (largest, held) : largest;
}

void forEachItemIDHeld (const Song& song, const std::function<void (std::uint64_t)>& each)
{
    const auto* values = song.getValues();

    if (values == nullptr)
    {
        forEachIDHeld (readTracksValue (song.getState()), each); // a song without values: its tree holds its tracks
        return;
    }

    forEachIDHeld (values->tracks.get(), each);

    forEachIDHeld (values->devices.get(), each);
    forEachIDHeld (values->sends.get(), each);
    forEachIDHeld (values->modulation.get(), each);

    for (const auto& entry : values->clips.get().clips)
        each (entry.second.id.getRaw());

    for (const auto& section : values->sections.get().sections)
        each (section.id.getRaw());
}

std::uint64_t findIDHeldTwice (const Song& song)
{
    std::set<std::uint64_t> seen, twice;
    forEachItemIDHeld (song,
                       [&seen, &twice] (std::uint64_t id)
                       {
                           if (id != 0 && ! seen.insert (id).second)
                               twice.insert (id);
                       });

    return twice.empty() ? 0 : *twice.begin();
}
} // namespace phazite
