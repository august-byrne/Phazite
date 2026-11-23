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

#include <algorithm>

namespace phazite
{
namespace
{
    // Whether the song holds its sends beside its tree: not yet (the sends' first piece, roadmap step S5.20: the tree is
    // their storage). forEachNameableIDHeld()'s line for them reads nothing until it is on.
    constexpr bool sendsHeldBesideTheTree = false;

    // Whether the song holds its devices, containers and chains beside its tree: not yet (the devices' first piece, roadmap
    // step S5.20: the tree is their storage). forEachNameableIDHeld()'s line for them reads nothing until it is on.
    constexpr bool devicesHeldBesideTheTree = false;
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
    largest = std::max (largest, findLargestIDIn (values->sections.get()));   // the sections' (each section's ID)
    largest = std::max (largest, findLargestIDIn (values->modulation.get())); // modulation's (each modulator's and macro's ID)
    return largest;
}

void forEachNameableIDHeld (const Song& song, const std::function<void (std::uint64_t)>& each)
{
    // Each area whose value holds items that something can name, a line each, in no order that matters: modulation's (each
    // modulator's and macro's: ModulationValue.h, what a MIDI link of a macro's value or of a modulator's parameter and a
    // macro's value's curve name), from its area's value. The sends' (each send's ID: SendsValue.h, what a curve, a MIDI link
    // and a modulation name a send's level by) only from their switch: until then the tree holds them (holdsID() of a SEND),
    // which the caller's walk of the tree finds, and the sends' switch turns sendsHeldBesideTheTree on, getSendsValue() then
    // reading the area's value.
    if (const auto* values = song.getValues())
        forEachIDHeld (values->modulation.get(), each);

    if constexpr (sendsHeldBesideTheTree)
        forEachIDHeld (getSendsValue (song), each);

    // The devices' (each device's, container's and chain's ID: DevicesValue.h), the same: the tree holds them until their
    // switch (holdsID() of a DEVICE, a CONTAINER and a CHAIN), which turns devicesHeldBesideTheTree on and reads the area's
    // value in place of the tree's.
    if constexpr (devicesHeldBesideTheTree)
        forEachIDHeld (readDevicesValue (song.getState()), each);
}

std::uint64_t countHeldID (std::uint64_t largest, std::uint64_t held) noexcept
{
    return held < schema::maxID ? std::max (largest, held) : largest;
}
} // namespace phazite
