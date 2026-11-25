// SPDX-License-Identifier: AGPL-3.0-or-later
// The IDs the song's values hold: the values hold the song's IDs (a clip's, its alias group's, its
// notes'), which the song's count of IDs must stay past (Schema.h: findNextID() of a
// song), and those of them that something can name, which what looks for the song's items finds (forEachNameableIDHeld()).
// Each area of values that holds IDs counts its own in its area's file (findLargestIDIn(): ClipsArea.h, NotesArea.h,
// SectionsArea.h, ModulationValue.h, SendsValue.h; forEachIDHeld(): ModulationValue.h, SendsValue.h), which
// findLargestIDHeld() and forEachNameableIDHeld() ask, a line each. Its own header, rather than SongValues.h's, so that what
// counts IDs needs nothing else of the values.
#pragma once

#include <cstdint>
#include <functional>

namespace phazite
{
class Song;

// The largest ID a value of the song holds, below schema::maxID (as findLargestID() counts a tree's): its clips' IDs, their
// alias groups' (clipLink) and their notes' IDs, its sections', its modulators' and macros', its sends', its devices' and its
// tracks'. 0 for none.
std::uint64_t findLargestIDHeld (const Song&);

// Each ID the song's values hold of an item that something can name (a device, a container, a chain, a send, a fader, a
// track, a modulator or a macro: what a curve, a MIDI link, a modulation or a send names), in no order, any one more than
// once. A clip and a section are no such item: the clips' and the sections' areas add none. What finds the items still there
// asks it (model/mixer/Mixer.cpp: forgetWhatNamesTheGone(), at every removal; model/versions/ApplySong.cpp:
// removeBrokenReferences(); tests/ReferencesTests.cpp).
void forEachNameableIDHeld (const Song&, const std::function<void (std::uint64_t)>& each);

// The larger of the largest counted so far and an ID held, where that is below schema::maxID (else the largest so far): how
// each area's count counts each ID it holds.
std::uint64_t countHeldID (std::uint64_t largest, std::uint64_t held) noexcept;

// Each ID an item of the song holds as its own, once for each item that holds it, in no order: each track's, its fader's and
// its VCA's, and the master's (as the tracks' area holds them), each
// device's, container's and chain's, each send's, each modulator's and macro's, each clip's and each section's. Not an alias
// group's (clipLink: its clips share it), nor a note's (its clip's notes count their own).
void forEachItemIDHeld (const Song&, const std::function<void (std::uint64_t)>& each);

// An ID two items of the song hold (forEachItemIDHeld()), the smallest of them; 0 for none, as the song holds after anything
// (Project.h: makeConsistent() leaves none, and every maker makes an ID anew). What the versions' passes assert of the song they
// made like another (model/versions/ApplySong.cpp), which may hold one twice meanwhile.
std::uint64_t findIDHeldTwice (const Song&);
} // namespace phazite
