// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracks and the master read by the tests by the song and IDs, as TestDeviceValues.h does for devices: the song holds its tracks
// as values, and what a test reads as a file holds them it reads from a whole copy (copyTrack()). What a test sets (a name, a
// volume, a height, a mute, a lane shown) it sets through the model's functions by ID (model/arrangement/internal/Tracks.h,
// model/mixer/internal/Mixer.h, model/automation/internal/Automation.h), and a state no model function makes it opens from a
// whole copy edited by hand (findTrackIn(), then makeSong()).
#pragma once

#include "TestHelpers.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

namespace phazite::test
{
// What the song holds of a track as its element, written whole (model/base/internal/WholeState.h: copyWholeSong(), the
// element found in it by the track's ID; the MASTER's for the master's ID): what a test reads a property (a name, a volume, a
// fader's ID, a key, a height, a mute, a frozen switch) or a child (its LANES) of. An invalid tree for a track the song
// hasn't. A copy: what a test writes into it changes nothing in the song. Its body is what the tracks' pieces change.
juce::ValueTree copyTrack (const Song&, TrackID);

// Whether the song holds a track (or is the master's): the presence check of a track that a version, an Undo or a merge
// brings back or takes away. Its body (today's model functions by ID) is what the tracks' pieces change.
bool hasTrack (const Song&, TrackID);

// One property of what the song holds of a track (copyTrack()), void where it hasn't it: for the one-liners.
juce::var getTrackProperty (const Song&, TrackID, const juce::Identifier&);

// A track's element (or the master's) in a whole copy of a song, or in a song's text read, by its ID: invalid for none. What
// a test changes by hand in it (a property out of its range) for a song opened from that copy
// (model/project/internal/Project.h: makeSong(), or TestSong::openSong()), as a song opens any file; a live song's own
// elements are never written so.
juce::ValueTree findTrackIn (const juce::ValueTree& wholeSong, TrackID);

// Whether the order of its tracks the song keeps (SongValues::getTrackOrder()) is the one its tracks make afresh
// (TracksValue.h: makeTrackOrder()), the lists holding together (holdsItsLists()).
// What the end of a performance gate, the long run and the undo check ask, whose asks are not checked one by one.
bool holdsExactTrackOrder (const Song&);
} // namespace phazite::test
