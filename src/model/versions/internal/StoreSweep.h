// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions' store swept (docs/design/versions.md: The store; roadmap S4.4), as git's gc and prune: this computer's own
// packs joined, leaving out the pieces nothing names that this computer wrote more than 30 days (keptByAge::dailyFor) before
// the keeping time (VersionStore::getKeepingTime(): now, but no more than a week past the last sweep); the bounces and this
// computer's edited messages of the versions that go with them. Each computer sweeps its own packs alone (PieceStore.h), so
// nothing another computer wrote, or needs, is ever deleted, whatever has arrived from it. The letting go is off in the
// product (sweepLetsGo, below): the packs are joined, losing nothing.
//
// What names a piece, on every computer (each computer's records are files of its own, all read, as far as they have
// arrived): every branch's last version and the versions its line reaches, the lines that went their own way on other
// computers, the songs left on branches, Local History's snapshots (thinned as of the keeping time), the songs before the
// merges that wait in the open song and in those, what left every branch within the 30 days before the keeping time
// (each computer's logs: an undone version, a deleted branch's versions and the song left on it, a branch reset away
// from), where no record of a computer's names a branch any more its last place by that computer's log, and every piece
// written within those 30 days; each walked down to every piece it names (SongPieces.h: findReferences()). A file that
// names something and doesn't read whole, a sync program's copy of one (VersionStore::areRecordsWhole()), or a named piece
// that can't be read (damaged, or not arrived) might name anything: nothing is left out then. Audio files are never
// touched: Clean Up Unused Audio decides those (model/project/UnusedAudio.h).
//
// Another kind of root (a version the project file names, its autosave's and its backups', roadmap S4.5) is added where
// findNamedPieces() names the open song's merge: whatever names a piece's name goes into the walk there.
//
// On the history's thread as it opens (VersionHistory::open()), before anything else reads or writes the store, and only
// when worth it: once a week, when what it would free is at least minimumFreed and a tenth of this computer's packs, or
// when this computer has more than maxPacks packs. A first sweep (none noted before) notes the time and leaves nothing out,
// so that a project gets a week's grace whatever its clock says. Any thread, one at a time for a folder.
#pragma once

#include "VersionStore.h"

#include <atomic>
#include <functional>
#include <map>
#include <set>

namespace phazite
{
struct Sweep
{
    bool joined = false;            // the packs were joined
    bool sure = true;               // every piece something names was read: false, nothing was left out
    juce::StringArray leftOut;      // the pieces left out
    juce::StringArray versionsGone; // the versions among them, whose bounces and edited messages went too
    juce::String why;               // not sure: what couldn't be read, as the view says it
};

// Whether the sweep lets go of what nothing names: off in the product (docs/known-bugs.md, "The versions' store lets go
// of nothing yet"), so that the store deletes nothing. With it off, a sweep still walks what is named (and says when it
// can't be sure), notes its time and joins this computer's packs by size, keeping every piece; no bounce or edited
// message goes; and no version is copied into this computer's own packs (adoptVersions(), VersionHistory::adoptLater()),
// as nothing another computer needs is deleted. Tests turn it on (SweepLetsGo, tests/TestSong.h) while they run, so that
// the letting go stays tested for the step that turns it on.
inline constexpr bool sweepLetsGo = false;
bool doesSweepLetGo();
void setSweepLetsGoForTests (bool);

// What a sweep frees at least to be worth rewriting packs a sync program uploads again: 4 MiB, and a tenth of this
// computer's packs. For tests: another floor while one is set (below 0: none).
inline constexpr juce::int64 minimumFreed = 4 * 1024 * 1024;
void setSweepFloorForTests (juce::int64 bytes);

// The names of the store's pieces that something names, as of a time (above: the keeping time), walked from what names
// them and from the pieces given too (those written within the 30 days before it); and whether every one of them, and
// every file naming them, could be read. openMerging: the open song's merge waiting (Schema.h: MERGING), invalid for none.
struct Named
{
    std::set<juce::String> pieces;
    bool sure = true;
    juce::String why; // not sure: what couldn't be read
};

Named findNamedPieces (VersionStore&, const juce::ValueTree& openMerging, const juce::StringArray& alsoFrom, juce::int64 asOf,
                       const std::function<bool()>& shouldStop);

// A sweep, if one is due (or whatever it frees, if forced): this computer's packs joined, what nothing names left out, and
// the bounces and this computer's edited messages of the versions that went, which it notes before the join and finishes
// after it, or at the next sweep if it is cut off (VersionStore::writeSwept()). Never in a folder a first save copies.
// Gives up between pieces, changing nothing, once shouldStop says so (as a waiter for the history's thread makes it: a
// stopped sweep runs at a later open).
Sweep sweepStore (VersionStore&, const juce::ValueTree& openMerging, int maxPacks, const std::function<bool()>& shouldStop,
                  bool force = false);

// Versions and songs made this computer's own (for when the sweep lets go: doesSweepLetGo()): of each version given (every
// version the history shows, those Recover Lost Versions offers, the merge waiting in the song's version and head), its
// record and every piece of its song, and of each song given (the merge's song before it, the songs branches name as left
// on them), every piece, that this computer's own packs don't hold, copied from another computer's packs into packs of its
// own (PieceStore::write()), a piece only with every piece it names, the merges whole songs hold followed too. So that
// whatever another computer sweeps of its own packs, this computer's history stays whole. What already has an own copy has
// all it names too (a save writes them together, and so does this), so only what lacks one is read.
//
// Takes from the front of what is left, a batch of a few megabytes at a call: written, then the call returns with the rest
// left, for another job. Stopped (shouldStop), it writes what it batched and returns with the rest. Never in a folder a
// first save copies (nothing done, the rest left).
struct Adoption
{
    juce::StringArray versions, songs; // still to copy
    bool isEmpty() const noexcept { return versions.isEmpty() && songs.isEmpty(); }
};

struct Adopted
{
    juce::StringArray unreadable; // what couldn't be copied whole: a piece of it not arrived, or damaged
    bool written = true;          // every batch went into a pack (false: a full disk; the rest left)
};

Adopted adoptVersions (VersionStore&, Adoption& left, const std::function<bool()>& shouldStop);
} // namespace phazite
