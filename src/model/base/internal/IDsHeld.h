// SPDX-License-Identifier: AGPL-3.0-or-later
// The IDs the song's values hold (roadmap step S5.20): what the song keeps beside its tree holds IDs that its tree no longer
// does (a clip's, its alias group's, its notes'), which the song's count of IDs must stay past (Schema.h: findNextID() of a
// song). Each area of values that holds IDs counts its own here, a line each, as it comes (the sections' with their switch).
// Its own header, rather than SongValues.h's, so that what counts IDs needs nothing else of the values.
#pragma once

#include <cstdint>

namespace phazite
{
class Song;

// The largest ID a value of the song holds, below schema::maxID (as findLargestID() counts a tree's): its clips' IDs, their
// alias groups' (clipLink) and their notes' IDs. 0 for none, and for a song that holds no values yet (Song::holdValues()).
std::uint64_t findLargestIDHeld (const Song&);
} // namespace phazite
