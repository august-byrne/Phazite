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
//           drum sampler), every value set by hand a parameter its device or modulator has, and a text Schema.h says every
//           element of its type holds (always, without words: a lane's parameter) there and not empty
//   key     two elements of a parent never with one key, as describeSongRules() keys them (two curves of one parameter,
//           two tempos at one beat, two TEMPOMAPs)
//   kind    what names one of a set of words names one of them: a track's kind, a clip's, a controller event's, a
//           modulator's type, a device's type (one of Phazite's own, or a plug-in), a write mode, a split
//   range   every number Schema.h gives a range written as a number in it (finite, and whole where it says int): a
//           channel's volume and pan, a tempo, a time signature, a note's pitch and velocity, places in beats (from 0 to
//           maxBeat), a note longer than nothing, and every parameter's value set by hand and every automation point's value in its parameter's range
//   path    no sound file a song names (an audio clip's, a device's) on a network share, nor leaving the project's folder
//           with ".."; a full path of any computer's is one Phazite writes itself; nothing for a song without a file
//   alias   the clips of an alias group (Schema.h: clipLink) hold the same notes and controllers' values (every property of
//           each NOTE and CONTROL, in the same order), are of one kind, and are two at least
//   values  of the song's values (model/base/internal/SongValues.h), each area on its own: the clips' lists agree with its
//           clips (holdsItsLists()) and no alias group is of one clip; each clip on a track of the song that holds clips
//           (not a folder or a return); every MIDI clip has a value of notes, an audio clip none, and no value belongs to a
//           clip that isn't a MIDI clip of the song's; the clips of an alias group hold ONE value of notes, or audio clips
//           ONE sound (the same, not two alike); each kind of the tempo map not empty and in the order of its beats; every
//           number within its description's range (Schema.h); and what each other area holds as its readers keep it
//   state   the state read back from the song's whole copy is the state the song holds
//   reopened the song's file's text, opened as a project opens it (the loader, then SongState.h: readSongTree()), writes
//           the same text, word for word, and that text opened again is the same state; but for a song whose IDs reach the
//           mark the loader renumbers from (Schema.h: renumberMark). Of findSongProblems() alone (a song as it would open),
//           never of findValueProblems(), whose songs hold what they keep while open
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"
#include "model/base/internal/SchemaDescription.h"
#include "model/base/internal/SongState.h"

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

// The problems of a song's file as it is, before the loader (a damaged file, a file from elsewhere): the rules that walk the
// song whole (place, id, key, kind, range, path, alias, and names but a parameter's, which only a song can tell), on the
// tree itself; for a path, the project's folder given, if any.
std::vector<std::string> findFileProblems (const juce::ValueTree& file, const schema::Tables& = schema::getTables(),
                                           const juce::File& projectFolder = {});

// The same, each problem a failed check.
void checkSong (const Song&);

// The problems of the rule values alone (above), which findSongProblems() finds too: for a song an edit, an undo or a redo
// left, which holds what it keeps while it is open (the lanes of devices gone, view state), and so is checked whole only as
// it would open (copyAsOpened()). With the rule state: the song whole read back from its file's form, and made of its file's
// text as a project opens it, is the song.
std::vector<std::string> findValueProblems (const Song&);
void checkValues (const Song&); // each a failed check

// What differs between two states of a song (SongState.h), an area at a time, each by its own == (identity aside): a line for
// each that differs, naming it ("clips", "tempo map"...); none for
// two states that hold the same song. What the rule values compares, of the state read back from the song's whole copy and the
// state the song holds (tests/SongStateTests.cpp too).
std::vector<std::string> findStateDifferences (const SongState&, const SongState&);

// The problems of the rule reopened alone (above), which findSongProblems() finds too.
std::vector<std::string> findReopenedProblems (const Song&);

// A copy of an edited song as a project opens it from its file: without what it keeps only while it is open (the lanes of
// devices taken away: SongIDs.h, which the check takes for names of nothing) and where playback starts; its curves as they
// are, as what takes a device away takes its curves along. For checking a song an edit, a merge or the fuzzer left. The song
// itself left as it is.
Song::Ptr copyAsOpened (const Song&);
} // namespace phazite::test
