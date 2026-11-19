// SPDX-License-Identifier: AGPL-3.0-or-later
// The song made like another, a version's, or a part of it, with the fewest edits (docs/design/versions.md: Making the song a
// version): each property that differs set, each element the song hasn't added where the other has it, each the other
// hasn't taken away, and the children put in its order, all through the song's undo history, in the caller's step. What
// isn't counted (model/base/internal/Schema.h: view state, how the song plays) stays as it is. What plays follows, as it
// does an undo: a plug-in whose state differs gets the other's (model/plugins/internal/PluginInstances.h), one whose state
// is the same isn't touched. Elements are matched by their keys (schema::getElementKey()). No ID is in the song twice
// after: what the other has in another place than the song (a clip on another track, a track in another folder) is taken
// from where the song has it. And the song's count of IDs ends past every ID it holds and past the other's count, so that
// what it took, from a song that counted further than it did, keeps its IDs to itself.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
// The whole song like the other.
void makeSongLike (Song&, const juce::ValueTree& other);

// The whole song the same as the other, what isn't counted too (its view state, how it plays): another branch's song, which
// the song becomes in place as it switches to it (VersionHistory::finishSwitch()). With the fewest edits, so that what both
// have stays as it is, and none of them an undo step: the song's undo history starts again, as what was in it belongs to
// the song it was. Its count of IDs is the larger of the two.
void makeSongTheSame (Song&, const juce::ValueTree& other);

// The song made what a copy of it taken earlier was, outside its undo history, which stays as it is: what changed in it
// without an undo step since (mute and solo, the loop, view state, a plug-in's settings stored, its count of IDs), once what
// the history holds is the copy's again (a version open put back: VersionsModel.cpp, backToNow()). With the fewest edits, so
// that only those are made; a song whose history doesn't hold the copy's would lose what that history refers to.
void makeSongAsCopied (Song&, const juce::ValueTree& copy);

// One track (a folder's own part, a return, or the master by its ID) like the other's, with what it holds and its devices'
// automation; one the other hasn't taken away, a folder without the tracks in it, which stay where it was, in their order,
// as ungrouping it leaves them; and one the song hasn't put back where the other has it (after the track before it there,
// in the same folder), a folder with the tracks in it that the song hasn't. An item that is on another track in the other
// song than in this one (a clip moved since) ends where the other has it, once, whichever of the two tracks is made like
// the other's: what the track held then comes back from where it is now, and what it holds now that the other holds
// somewhere else goes there, as it is now; only where the song has nothing to hold it there (that track deleted since) does
// it go with the rest, for that track's own line to bring back. False if neither has it.
bool makeTrackLike (Song&, TrackID, const juce::ValueTree& other);

// The song's own like the other's: its own properties, its tempo map, its sections, its chord lane and the order of the
// tracks both have.
void makeSongOwnLike (Song&, const juce::ValueTree& other);

// What a part of the song names that isn't there any more, taken away: a sidechain from a track that is gone, a modulation
// of a device that is gone, a send to a return that is gone, and a pad's track whose drum sampler is gone, as an edit takes
// them away (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()); and a sidechain that would hear its own track's sound
// round a loop (model/mixer/internal/Sidechain.h: forgetSidechainLoops()), which a line taken from one song and another
// from another can make together. Every apply of a version, a line, a merge or a merge's clash runs it, in its step; how
// many it took away (of the pads' tracks, none).
int removeBrokenReferences (Song&);
} // namespace phazite
