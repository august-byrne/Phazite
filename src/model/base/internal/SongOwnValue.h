// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its own, as values (roadmap step S5.20, the song's own): its root element's properties (Schema.h:
// PHAZITE's outline, notes, idNumbering, patternsKeepTheirSpeed, the loop's, playStart, metronome, punch, lastInput, the
// playlist's grid, versionsBranch, versionsBase) and its small elements (LASTKEY, PIANOROLL, MERGING), in three values by how
// the song writes them: what only an undo step writes (SongOwnValue, plain: an Undo puts it back whole), what a step and a
// write outside the history both write (SongStanding, keyed: an Undo puts back only the entries its step changed, as the
// tree's Undo of a property does), and what no step writes (SongOutside: state and view, never in the history). nextID is no
// value of these: a count, which the song holds itself (Song.h: makeID()).
//
// The song holds them as areas of its values (SongValues.h: songOwn, songStanding, songOutside), its tree none of them: the
// getters below read each area's value as it is, and every reader of the song's own reads through them; the setters below
// are their writers' one way to change them, each setting the area's next value once; and what listens to the song hears
// them change from the values' listener (SongValues.h: songOwnChanged(), songStandingChanged(), songOutsideChanged(), told
// by each area's teller: SongOwnArea.h). The conversion below is the one place that reads and writes these names: what the
// song takes in as it opens (SongOwnArea.h, as it is made of its state) and what a whole copy is written from (WholeState.h).
#pragma once

#include "model/base/internal/ValueArea.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <utility>

namespace phazite
{
class Song;

// A merge waiting (Schema.h: MERGING), as its element holds it: text each, never read here. The versions read it as what
// anyone may have written (model/versions/internal/VersionHistory.h: readMergeState()), as they read the element: one that
// doesn't hold together is no merge.
struct MergingRecord
{
    juce::String version {}, head {}, before {}, chosen {}, renumbered {}, translated {}, crossed {};

    bool operator== (const MergingRecord&) const = default;
};

// What the song holds of its own that only its undo history changes (Schema.h: PHAZITE's song properties, MERGING). Plain: an
// Undo puts the whole value back. Each text empty for none.
struct SongOwnValue
{
    juce::String outline {};                 // the outline applied last (Outline::id)
    juce::String notes {};                   // the project's notes, their lines split by "\n"
    juce::String idNumbering {};             // the numbering the IDs are in, none until the loader first gives them anew
    bool patternsKeepTheirSpeed = false;     // a tempo change leaves the patterns at their speed
    std::optional<MergingRecord> merging {}; // a merge waiting

    bool operator== (const SongOwnValue&) const = default; // no floating point
};

// A key as the song saves it (Schema.h: keyRoot, keyScale): the root, 0 to 11, and the scale's word, empty for none (read as
// major). Base's own pair: model/notes/internal/Notes.h makes a Key of it.
struct SavedKey
{
    int root = 0;
    juce::String scale {};

    bool operator== (const SavedKey&) const = default;
};

// What the song holds of its own that both a step and a write outside one change: the key set last (LASTKEY), which a key set
// on a track writes outside the history and a version applied or a merge's choice in a step; and where the song stands in its
// versions (versionsBranch, versionsBase), which the versions write outside the history and Take the Newer Versions in its
// step. Keyed (ValueArea.h: undoesByEntry), each field an entry of its own, so that an Undo puts back only what its step
// changed. Each text empty for none.
struct SongStanding
{
    static constexpr bool undoesByEntry = true;

    std::optional<SavedKey> lastKey {}; // none until a key was set
    juce::String versionsBranch {};     // none before the first version
    juce::String versionsBase {};

    bool operator== (const SongStanding&) const = default;
};

// The entries that differ between two values of the song's standing, each with its before and after; and a value with exactly
// those moved back or forward: the keyed value's functions (ValueArea.h).
struct SongStandingChanges
{
    std::optional<std::pair<std::optional<SavedKey>, std::optional<SavedKey>>> lastKey {};
    std::optional<std::pair<juce::String, juce::String>> versionsBranch {}, versionsBase {};
};

SongStandingChanges findChanges (const SongStanding& before, const SongStanding& after);
SongStanding withChanges (SongStanding now, const SongStandingChanges&, ChangeDirection);

// The step every grid starts with ("1/4 beat"), one text made once: what a grid saved without its step reads as.
const juce::String& getDefaultGridStep();

// A grid's settings as the song saves them (Schema.h: gridSnap, gridAdaptive, gridOffset, gridStep, gridTriplets), the step as
// its saved word. model/arrangement/internal/Grid.h makes the grid of it (loadGridSettings()).
struct SavedGrid
{
    bool snap = true, adaptive = true, triplets = false;
    int offset = 0;
    juce::String step = getDefaultGridStep();

    bool operator== (const SavedGrid&) const = default;
};

// What the song keeps of how it plays and shows (Schema.h: state and view, uncounted): never in the history.
struct SongOutside
{
    bool loop = false, metronome = false;
    std::optional<bool> loopFollowsSong {};         // none until it changed (written then, true too): none follows
    std::optional<double> loopStart {}, loopEnd {}; // a range looped on purpose
    double playStart = 0.0;                         // in beats; not written at 0
    std::optional<bool> punch {};                   // once set, false too (as setPunchOn() writes it)
    juce::String lastInput {};                      // the input chosen last, empty for none
    std::optional<SavedGrid> playlistGrid {};       // PHAZITE's five grid properties: none until first saved
    std::optional<SavedGrid> pianoRollGrid {};      // PIANOROLL: none until first written

    bool operator== (const SongOutside&) const; // written out: the doubles compared exactly (juce::exactlyEqual)
};

//==============================================================================
// The conversion: the one place that reads and writes these names.

// What a song's root (PHAZITE) holds of each, as Phazite's code always read it (each property's own reading: a switch as a
// var's bool, a number as a var's, a text as it is): for the tests and the check. Nothing but defaults for another element.
SongOwnValue readSongOwnValue (const juce::ValueTree& phazite);
SongStanding readSongStanding (const juce::ValueTree& phazite);
SongOutside readSongOutside (const juce::ValueTree& phazite);

// The same as the song holds it, each field by its description in Schema.h, by the rule the loader applies
// (model/project/internal/Project.h: makeConsistent()): a numbering's and a version's names only of their hexadecimal digits
// (hexDigits()), else none; the loop's range, where playback starts and a grid's offset within their ranges (a number that
// is no finite one none, where playback starts 0, an offset made whole), and the loop's range only with both its ends, the
// end past the start, else none; the key's root within 0 to 11, made whole, none where it is no finite number, its scale one
// of its words, else none; a grid only where it holds one of its properties as its description keeps it (a step that is one
// of its words, an offset that is a number), its step one of its words, else the default. What the getters read. A field
// whose description bounds nothing (the texts, the switches) is read as Phazite's code always read it.
SongOwnValue readHeldSongOwn (const juce::ValueTree& phazite);
SongStanding readHeldSongStanding (const juce::ValueTree& phazite);
SongOutside readHeldSongOutside (const juce::ValueTree& phazite);

// The three values written into a song's root, which holds none of them: its properties as Phazite's writers write them
// (outline, notes, idNumbering, the names and lastInput only where not empty; patternsKeepTheirSpeed, loop and metronome only
// while on; loopFollowsSong and punch where held, false too; loopStart and loopEnd where held; playStart only past 0; all five
// of the playlist's grid where it is saved), then LASTKEY (where a key is held), PIANOROLL (where its grid is) and MERGING
// (where a merge waits), each appended.
void writeSongOwnInto (juce::ValueTree& phazite, const SongOwnValue&, const SongStanding&, const SongOutside&);

// The same, the root's properties alone (none of its elements): what the versions' skeleton of the song takes
// (model/versions/ChangesWorker.cpp: makeSkeleton()).
void writeSongOwnPropertiesInto (juce::ValueTree& phazite, const SongOwnValue&, const SongStanding&, const SongOutside&);

// A song's root as the song holds it, written as Phazite writes that, so that the root read as held and written back is
// itself: the loader's rule (makeConsistent(), once each number is within its range, each word one of its words and each
// element held once), the held readers' by the same conversion. Of the root's properties and its first LASTKEY, PIANOROLL and
// MERGING, each property the conversion knows as writeSongOwnInto() writes what readHeld...() read: a switch written off
// (loop, metronome, patternsKeepTheirSpeed), an empty text (notes, outline, lastInput, a merge's), playStart at 0 and a
// loop's range without both ends or not past its start taken out; a grid with fewer than its five properties given the
// others at their defaults; and the element that holds nothing it is for taken out (a LASTKEY without a root, a PIANOROLL
// without a grid's property). What the conversion doesn't know (nextID, a later build's) stays. Whether any changed.
bool keepSongOwnAsHeld (juce::ValueTree phazite);

// A MERGING element's record, each text as it holds it (empty for none); and the element of a record, as the versions write
// one: each text only where not empty (one without its version, head or before is no merge: VersionHistory.h,
// readMergeState(), which the versions show and Cancel Merge takes out, kept as it is).
MergingRecord readMergingRecord (const juce::ValueTree& merging);
juce::ValueTree writeMergingElement (const MergingRecord&);

// One grid's saved settings as Phazite's code always read them (Grid.h: loadGridSettings()): each where held, else its default;
// what a tree of a file's grid is read by. The defaults for an invalid tree.
SavedGrid readSavedGrid (const juce::ValueTree& gridHolder);

// What the three values hold of a song's root, by name: a property of its own (Schema.h: PHAZITE's outline, notes,
// idNumbering, patternsKeepTheirSpeed, the versions' two names, the loop's four, playStart, metronome, punch, lastInput and
// the playlist's grid; not nextID, a count, nor a property Phazite doesn't know); and an element of its own (LASTKEY,
// PIANOROLL, MERGING), by its type. What the song reads as it is made of a file's state
// (SongOwnArea.h) and what the passes over a song's elements leave to the values (model/versions/ApplySong.cpp).
bool isSongOwnProperty (const juce::Identifier& property);
bool isSongOwnElement (const juce::Identifier& type);

// Whether Phazite knows a property of a song's root, by the descriptions: one they put on PHAZITE (Schema.h: the song's own, and
// its count of IDs, nextID); and an element there, by its type (Schema.h: isKnownType()). What it doesn't know at the root, the
// one rule of both: dropped as the song opens (the loader: model/project/internal/Project.h, makeConsistent()), and left out of a
// merge, neither merged nor compared (model/versions/SongMerge.cpp: mergeSongs()), so that no clash is made of what the song
// can't hold, and no change listed of it (model/versions/SongDiff.cpp: findSongChange()).
bool isKnownRootProperty (const juce::Identifier& property);
bool isKnownRootElement (const juce::Identifier& type);

//==============================================================================
// The getters: each value as the song holds it, its area's (SongValues.h: songOwn, songStanding, songOutside), by reference
// into it, valid until the area's next change: what the readers of the song's own read, a tick's poll among them (the top
// bar's loop, the time line's punch), with nothing allocated and nothing read but the one value. A caller that keeps one
// across a write keeps a copy (auto, not auto&).
const SongOwnValue& getSongOwn (const Song&);
const SongStanding& getSongStanding (const Song&);
const SongOutside& getSongOutside (const Song&);

//==============================================================================
// The setters: each value's writers' one way to change it, the next value worked out and set once into its area. None begins
// an undo step. A value equal to what the song holds is no change: nothing set, no action in the history, nothing told.
//   - setSongOwn(): in the song's undo history (the step in progress: an outline applied, the notes, patterns' speed, a merge
//     taken or a clash chosen, a version applied), or outside it while the song is written outside it (Song::OutsideHistory).
//   - setSongStanding(): the same (Take the Newer Versions' base, a version applied's key set last).
//   - storeStandingOutsideHistory(): outside the history whatever is open (a key set on a track, the key set last, where the
//     song stands in its versions as the versions move it): taken into the action of a step still open that changed the
//     standing, but for the entries that step changed, which keep its before and after (ValueArea.h: keyed).
//   - setSongOutside(): outside the history, always (the loop, the play start, the metronome, punch, the input chosen last,
//     the grids).
// In SongOwnArea.cpp.
void setSongOwn (Song&, SongOwnValue);
void setSongStanding (Song&, SongStanding);
void storeStandingOutsideHistory (Song&, SongStanding);
void setSongOutside (Song&, SongOutside);
} // namespace phazite
