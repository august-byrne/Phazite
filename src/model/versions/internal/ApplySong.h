// SPDX-License-Identifier: AGPL-3.0-or-later
// The song made like another, a version's, or a part of it, with the fewest edits (docs/versions.md: Making the song a
// version): each property that differs set, each element the song hasn't added where the other has it, each the other
// hasn't taken away, and the children put in its order, all through the song's undo history, in the caller's step. What
// isn't counted (model/base/internal/Schema.h: view state, how the song plays) stays as it is. What plays follows, as it
// does an undo: a plug-in whose state differs gets the other's (model/plugins/internal/PluginInstances.h), one whose state
// is the same isn't touched. Elements are matched by their keys (schema::getElementKey()).
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
// The whole song like the other.
void makeSongLike (Song&, const juce::ValueTree& other);

// One track (a folder's own part, a return, or the master by its ID) like the other's, with what it holds and its devices'
// automation; one the other hasn't taken away, and one the song hasn't put back where the other has it (after the track
// before it there, in the same folder). False if neither has it.
bool makeTrackLike (Song&, TrackID, const juce::ValueTree& other);

// The song's own like the other's: its own properties, its tempo map, its sections, its chord lane and the order of the
// tracks both have.
void makeSongOwnLike (Song&, const juce::ValueTree& other);

// What a part of the song names that isn't there any more, taken away: a sidechain from a track that is gone, a modulation
// of a device that is gone (a send to a return that is gone stays, as the song keeps one for an undo that brings the
// return back). In the caller's step; how many it took away.
int removeBrokenReferences (Song&);
} // namespace phazite
