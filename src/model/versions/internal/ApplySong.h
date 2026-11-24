// SPDX-License-Identifier: AGPL-3.0-or-later
// The song made like another, a version's, or a part of it, with the fewest edits (docs/design/versions.md: Making the song a
// version): each property that differs set, each element the song hasn't added where the other has it, each the other
// hasn't taken away, and the children put in its order, all through the song's undo history, in the caller's step. What
// isn't counted (model/base/internal/Schema.h: view state, how the song plays) stays as it is. What plays follows, as it
// does an undo: a plug-in whose state differs gets the other's (model/plugins/internal/PluginInstances.h), one whose state
// is the same isn't touched. Elements are matched by their keys (schema::getElementKey()); the tracks' clips, in a pass of their
// own once the rest is made like the other's, by their IDs and values through the clips' functions
// (model/arrangement/internal/Clips.h), whatever stores them, each track's in the other's order; and the song's curves, in a
// pass of their own, by their parameters through automation's functions (model/automation/internal/Automation.h: setCurveValue(),
// eraseCurve()), a curve's points by their places, the whole song's in the other's order; and the song's modulators and
// macros, in a pass of their own, by their IDs through modulation's setters (model/modulation/internal/Modulation.h:
// setModulator(), eraseModulator(), setModulatorOrder()), each owner's in the other's order; and the song's tracks, folders,
// returns and master, first, by their IDs through the tracks' setters (model/arrangement/internal/Tracks.h: setTrack(),
// setNewTracks(), setTrackPlace(), eraseTrack()), a track the other has in another folder moved there,
// keeping what it holds. No ID is in the song twice after: what the other has in another place than the song (a clip on
// another track) is taken from where the song has it. And the song's count of IDs ends past every ID it holds and past the
// other's count, so that
// what it took, from a song that counted further than it did, keeps its IDs to itself.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

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

// One track (a folder's own part, a return, or the master: named by either song's ID for it, the master is found as the
// master in each, as a version may give it another ID) like the other's, with what it holds and its devices'
// automation; one the other hasn't taken away, a folder without the tracks in it, which stay where it was, in their order,
// as ungrouping it leaves them; and one the song hasn't put back where the other has it (after the track before it there,
// in the same folder), a folder with the tracks in it that the song hasn't. An item that is on another track in the other
// song than in this one (a clip moved since) ends where the other has it, once, whichever of the two tracks is made like
// the other's: what the track held then comes back from where it is now, and what it holds now that the other holds
// somewhere else goes there, as it is now; only where the song has nothing to hold it there (that track deleted since) does
// it go with the rest, for that track's own line to bring back. False if neither has it; and false, the song as it was (its
// count of IDs too), where it would give a track an ID the song holds now for another item (givesTrackIDsHeldElsewhere()).
bool makeTrackLike (Song&, TrackID, const juce::ValueTree& other);

// Whether a track's line (makeTrackLike()) would give a track an ID (its own, its fader's or its VCA's) that the song holds now
// for another item than that track as it is (another track, a fader or a VCA, a device, a container or a chain, a send, a
// modulator or a macro, a clip, a section): one the line's passes may not reach, so that the song would hold the ID twice.
// Two lines that counted their IDs on from the same place make it (another computer's branch switched to or restored; a merge
// gives IDs anew). What a line's restore refuses then, saying why (VersionsModel.cpp: takeBack(), takeInRestoreLine()); the
// whole song's passes take such an ID (makeSongLike()).
bool givesTrackIDsHeldElsewhere (const Song&, TrackID, const juce::ValueTree& other);

// The song's own like the other's: its own properties, its tempo map, its sections, its chord lane and the order of the
// tracks both have.
void makeSongOwnLike (Song&, const juce::ValueTree& other);

// What a part of the song names that isn't there any more, taken away: a sidechain from a track that is gone, a modulation
// of a device that is gone, a send to a return that is gone, and a pad's track whose drum sampler is gone, as an edit takes
// them away (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone(), which takes the MIDI links and the curves of what is
// gone too, uncounted); and a sidechain that would hear its own track's sound
// round a loop (model/mixer/internal/Sidechain.h: forgetSidechainLoops()), which a line taken from one song and another
// from another can make together. Every apply of a version, a line, a merge or a merge's clash runs it, in its step; how
// many it took away (of the pads' tracks, none).
int removeBrokenReferences (Song&);

// How many writes the passes above made of what the devices' store holds (ApplySong.cpp: makeDeviceStoreLike(): a plug-in's
// state, program, format ID, version and values set by hand, a chain's mute and solo, a window's place), by every song since
// the program started: for the tests, which check that a second pass writes nothing. Any thread.
std::uint64_t countDeviceStorePassWrites() noexcept;
} // namespace phazite
