// SPDX-License-Identifier: AGPL-3.0-or-later
// The IDs the song's values hold (roadmap step S5.20): what the song keeps beside its tree holds IDs that its tree no longer
// does (a clip's, its alias group's, its notes'), which the song's count of IDs must stay past (Schema.h: findNextID() of a
// song), and those of them that something can name, which what looks for the song's items finds (forEachNameableIDHeld()).
// Each area of values that holds IDs counts its own in its area's file (findLargestIDIn(): ClipsArea.h, NotesArea.h,
// SectionsArea.h; forEachIDHeld(): ModulationValue.h, SendsValue.h), which findLargestIDHeld() and forEachNameableIDHeld()
// ask, a line each. Its own header, rather than SongValues.h's, so that what counts IDs needs nothing else of the values.
#pragma once

#include <cstdint>
#include <functional>

namespace phazite
{
class Song;

// The largest ID a value of the song holds, below schema::maxID (as findLargestID() counts a tree's): its clips' IDs, their
// alias groups' (clipLink) and their notes' IDs, and its sections'. 0 for none, and for a song that holds no values yet
// (Song::holdValues()).
std::uint64_t findLargestIDHeld (const Song&);

// Each ID the song's values hold of an item that something can name (a device, a container, a chain, a send, a fader, a
// track, a modulator or a macro: what a curve, a MIDI link, a modulation or a send names), which the song's tree may not
// hold, in no order, any one more than once. A clip and a section are no such item: the clips' and the sections' areas add
// none. Modulation's modulators and macros, and the sends, join as they leave the tree (each area's switch turns its line on:
// IDsHeld.cpp); while the tree holds them, its walk finds them, and this reads nothing. What finds the items still there
// asks it beside the tree's (model/mixer/Mixer.cpp: forgetWhatNamesTheGone(), at every removal; model/versions/
// ApplySong.cpp: removeBrokenReferences()), so that it finds what the tree no longer holds as each area leaves it
// (tests/ReferencesTests.cpp guards it across the switch).
void forEachNameableIDHeld (const Song&, const std::function<void (std::uint64_t)>& each);

// The larger of the largest counted so far and an ID held, where that is below schema::maxID (else the largest so far): how
// each area's count counts each ID it holds.
std::uint64_t countHeldID (std::uint64_t largest, std::uint64_t held) noexcept;
} // namespace phazite
