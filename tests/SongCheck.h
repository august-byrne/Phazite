// SPDX-License-Identifier: AGPL-3.0-or-later
// A song checked for consistency, as any song Phazite loads must be, whoever made its file: what the model can't make, a
// file from elsewhere could hold. Used after every load in the fuzzer (Fuzzer.h, FuzzTests.cpp), on hand-made projects
// that parse but don't hold together (InconsistentProjectTests.cpp), and on the songs the random edits make
// (RandomEdits.h). Where each element goes, the ranges, the words and the hexadecimal ids are read from Schema.h's
// description of the song (schema::getTables()), as the loader reads them; each other rule is written on its own, rather
// than from the code that keeps the song so (Project.cpp: makeSong()), so that the check is a second statement of it.
// tests/golden/format/song-schema.json records the description, so that a change to a range or a word shows there.
//
// The rules, each problem a line that starts with its name:
//   place   each element of a type Phazite knows in a parent Schema.h puts it in (a CLIP on a track or a return, a NOTE
//           in a MIDI clip...); one of a type it doesn't know is left alone, with everything in it (SongIDs.h:
//           isKnownElementType()), as the format keeps it for a later build or another tool
//   id      every item's ID once in the song, a note's once in its clip, and the song's next ID past them (SongIDs.h)
//   names   what names an item names one the song has (a sidechain, a lane's, a curve's and a modulation's device, a pad's
//           drum sampler), and every value set by hand a parameter its device or modulator has
//   key     two elements of a parent never with one key, as describeSongRules() keys them (two curves of one parameter,
//           two tempos at one beat, two TEMPOMAPs)
//   kind    what names one of a set of words names one of them: a track's kind, a clip's, a controller event's, a
//           modulator's type, a device's type (one of Phazite's own, or a plug-in), a write mode, a split
//   range   every number Schema.h gives a range written as a number in it (finite, and whole where it says int): a
//           channel's volume and pan, a tempo, a time signature, a note's pitch and velocity, places in beats (from 0 to
//           maxBeat), a note longer than nothing, and every parameter's value set by hand and every automation point's value in its parameter's range
//   path    no sound file a song names (an audio clip's, a device's) on a network share, nor leaving the project's folder
//           with ".."; a full path of any computer's is one Phazite writes itself; nothing for a song without a file
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"
#include "model/base/internal/SchemaDescription.h"

#include <string>
#include <vector>

namespace phazite::test
{
// The furthest place in beats a song holds anything at: a million beats, a few days of music at any tempo.
inline constexpr double maxBeat = 1.0e6;

// Each problem with a song, a line each, in the order found; none for a consistent song. With tables, the places, ranges,
// words and hexadecimal ids of those (a test's changed copy of the description) rather than the description's.
std::vector<std::string> findSongProblems (const Song&);
std::vector<std::string> findSongProblems (const Song&, const schema::Tables&);

// The same, each problem a failed check.
void checkSong (const Song&);
} // namespace phazite::test
