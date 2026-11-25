// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its own (its root's properties and small elements: the outline, the notes, the loop, the count of IDs,
// the key set last, a merge waiting), set and read by the tests through the model: the song holds them as values, and one Phazite
// doesn't know can't be set on it. What a test needs as a plain change to the song (a step to undo, an unsaved change, a change
// no step makes) is a probe here, each a real, counted change the model makes; what it reads as the file would hold it it reads
// from a whole copy (TestHelpers.h: copyWholeSong()).
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"

namespace phazite::test
{
//==============================================================================
// One counted change in the song's history: the step probe, the outline applied last (model/arrangement/internal/
// Outlines.h: applyOutline(), of an outline of no sections whose id is the text; getAppliedOutline()). It joins the step in
// progress (the test's own, a gesture's, a trial's, a command's) and is one change of the song's own. As applying an outline
// does, it takes the song's sections away: for a song without sections (REQUIRE'd), as a new song and a new project are.
void setStepProbe (Song&, const juce::String& text);

// The step probe's text: empty for none.
juce::String getStepProbe (const Song&);

// A counted change in an undo step of its own ("Change Probe"), n times one after the other, each a probe text not set
// before: the plain change for a test that needs one (an unsaved change, a step to undo, a song that differs).
void changeSongInStep (Song&, int n = 1);

// The step probe set to a text in an undo step of its own ("Change Probe"): a change a test reads back (from the project
// saved and opened again, from a copy).
void changeSongInStep (Song&, const juce::String& probe);

//==============================================================================
// Several counted values in the song's history, independent of each other and of the step probe, for a test that needs more
// than one: the name of each of the song's tracks (model/arrangement/internal/Tracks.h: setTrackName()), by the track's
// place among them (getTrackIDs()), which the test made first (ensureTracks()). Setting one joins the step in progress.
void setTrackProbe (Song&, int place, int value);

// The value a track's probe was set to: -1 while its name is none of a probe's.
int getTrackProbe (const Song&, int place);

//==============================================================================
// A change to the project that no undo step makes, which makes it unsaved and the versions count a change, and which no
// Undo takes back: a change a plug-in made in its own window (Song::markChangedOutsideHistory()). The song's file doesn't
// hold it: a test whose change a save must write uses setKeptOutsideHistory().
void changeOutsideHistory (Song&);

// A counted change that no undo step makes, which a save writes and the project opened again reads back: the key set last
// (model/notes/internal/Notes.h: setLastKey(), its root the value, 0 to 11, the scale major).
void setKeptOutsideHistory (Song&, int value);

// The value setKeptOutsideHistory() set, as the song holds it: -1 for none.
int getKeptOutsideHistory (const Song&);

// A change the song's history should hold, written round it, as no model function writes one: a section added while the song
// is written outside its history (Song::OutsideHistory, as the versions' passes write a song). For the tests of the undo
// check (UndoCheck.h), which finds it.
void changeRoundTheHistory (Song&);

//==============================================================================
// The merge waiting in the song, as a whole copy of it holds its element (Schema.h: MERGING): invalid for none. A copy: what
// a test writes into it changes nothing in the song.
juce::ValueTree copyWaitingMerge (const Song&);

// Whether a merge waits in the song (an element of one, which may not hold together).
bool hasWaitingMerge (const Song&);

// The merge waiting in the song with one of its values set by hand, as a file from anyone may hold it, set in the song outside
// its history (model/versions/internal/VersionHistory.h: setWaitingMerge()), which it lets go of first, as a song opened from
// such a file has none.
void changeWaitingMerge (Song&, const juce::Identifier& property, const juce::var& value);
} // namespace phazite::test
