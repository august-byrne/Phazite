// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its own is three areas of its values (model/base/internal/
// SongOwnArea.h), with its count of IDs (Song.h: makeID(), which tells no one), which a whole copy writes in their places;
// the keyed standing undoes by entry, a key set outside the history surviving the Undo of a step that changed another entry;
// each writer's step is exact (UndoCheck); a file with what Phazite doesn't know at its root opens and saves without it, and
// a version with it, switched to, lists no change; a merge waiting set by hand in a song with nothing to undo is saved with
// it; and how the song plays and shows, changed after a save, leaves the project saved, and is the file's again as it opens.
// With the tracks' area beside it: the count of IDs is past every track's IDs, which only that area holds; and a project
// opened opens again as the same song, and counts on across Undo and Redo.
//   phazite_tests.exe --test-case="Song's own areas*"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestSongOwn.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/GridSettings.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnArea.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/NoteValues.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace
{
namespace schema = phazite::schema;

phazite::BeatPosition at (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

juce::String repeat (char c, int times)
{
    return juce::String::repeatedString (juce::String::charToString ((juce::juce_wchar) c), times);
}

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (phazite::test::waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (phazite::test::waitForVersionHistory (context));
    return phazite::getLastVersion (context)->id;
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}
} // namespace

//==============================================================================
TEST_CASE ("Song's own areas: a whole copy writes the song's own and its count of IDs, each in its place")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);

    // Each kind set: its own value, its standing, how it plays and shows (the piano roll's grid an element of its own). The
    // merge waiting first, set by hand in a song with nothing to undo yet (VersionHistory.h: setWaitingMerge()).
    phazite::setWaitingMerge (
        song, phazite::writeMergingElement ({ .version = repeat ('a', 64), .head = repeat ('b', 64), .before = repeat ('c', 64) }));
    phazite::setProjectNotes (s.context, "Notes");
    phazite::setLastKey (song, { .root = 3, .scale = phazite::Scale::minor });
    phazite::toggleLoop (song);
    auto grid = phazite::loadGridSettings (song, phazite::GridOwner::pianoRoll);
    grid.snap = ! grid.snap;
    phazite::saveGridSettings (song, phazite::GridOwner::pianoRoll, grid);
    phazite::test::checkSong (song);

    // A whole copy writes each, and the count, each element in its place in the description's order (Schema.h): LASTKEY
    // first, PIANOROLL after TEMPOMAP, MERGING after MASTER.
    const auto whole = phazite::test::copyWholeSong (song);
    CHECK (whole[schema::notes].toString() == "Notes");
    CHECK ((bool) whole[schema::loop]);
    CHECK (phazite::readID (whole[schema::nextID]) == phazite::getNextID (song));
    const auto indexOf = [&whole] (const juce::Identifier& type) { return whole.indexOf (whole.getChildWithName (type)); };
    CHECK (indexOf (schema::LASTKEY) == 0);
    CHECK (indexOf (schema::TEMPOMAP) == 1);
    CHECK (indexOf (schema::PIANOROLL) == 2);
    CHECK (indexOf (schema::TRACKS) == 3);
    CHECK (indexOf (schema::MASTER) == 4);
    CHECK (indexOf (schema::MERGING) > indexOf (schema::MASTER));

    // An ID made: the count moves on, telling no one (no undo step).
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    const auto next = phazite::getNextID (song);
    CHECK (song.makeID() == next);
    CHECK (phazite::getNextID (song) == next + 1);
    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
    CHECK (phazite::readID (phazite::test::copyWholeSong (song)[schema::nextID]) == next + 1);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song's own areas: a merge waiting set by hand in a song just opened or made is saved with it, and is the song's again "
           "as it opens")
{
    const phazite::MergingRecord merge { .version = repeat ('a', 64),
                                         .head = repeat ('b', 64),
                                         .before = repeat ('c', 64),
                                         .chosen = "21" };

    // A song made: its whole copy holds the merge, and a song made from that copy holds it as its own.
    {
        const auto song = phazite::createSong();
        REQUIRE_FALSE (song->getUndoManager().canUndo());
        phazite::setWaitingMerge (*song, phazite::writeMergingElement (merge));
        const auto made = phazite::makeSong (phazite::test::copyWholeSong (*song));
        REQUIRE (made != nullptr);
        CHECK (phazite::getSongOwn (*made).merging == std::optional (merge));
        phazite::test::checkSong (*made);
    }

    // A song just opened: saved, and opened again, the file's merge.
    phazite::test::TestSong s;
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    auto& opened = *phazite::findSong (s.context);
    REQUIRE_FALSE (opened.getUndoManager().canUndo());
    phazite::setWaitingMerge (opened, phazite::writeMergingElement (merge));
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    auto& reopened = *phazite::findSong (s.context);
    CHECK (phazite::getSongOwn (reopened).merging == std::optional (merge));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    phazite::test::checkSong (reopened);

    // In a song with a step to undo: asserted, as that step's Undo would put back the song's own without it (a plain area).
    phazite::setProjectNotes (s.context, "Notes");

    const phazite::test::ExpectedAssertion withSteps ("VersionHistory.cpp");
    phazite::setWaitingMerge (reopened, {});
#if JUCE_DEBUG
    CHECK (withSteps.getCount() == 1);
#endif
}

//==============================================================================
TEST_CASE ("Song's own areas: the standing undoes by entry: a key set outside the history survives the Undo of a step that set the "
           "versions' names, and of one that set the key, an outside write to an entry the step changed is the step's")
{
    const auto song = phazite::createSong();
    phazite::setLastKey (*song, { .root = 1 });
    song->getUndoManager().clearUndoHistory();
    const auto base = repeat ('a', schema::versionsBaseDigits);
    const auto firstBranch = repeat ('d', schema::versionsBranchDigits);

    // The versions' names set in a step (VersionsModel.cpp: takeNewerVersions() sets the base; a version applied both, as
    // here: ApplySong.cpp), then another step, so that the key set last outside the history after it lands outside the first
    // step's action (ValueArea.h: no longer the history's current step). The first step's Undo, which finds the standing
    // changed since (the key), moves only its own entries back (withChanges()): the names go, the key stays; its Redo the
    // names again.
    {
        const phazite::UndoStep step (*song, "Take the Newer Versions");
        auto next = phazite::getSongStanding (*song);
        next.versionsBase = base;
        next.versionsBranch = firstBranch;
        phazite::setSongStanding (*song, std::move (next));
    }

    phazite::test::changeSongInStep (*song, "between");
    phazite::setLastKey (*song, { .root = 5 });
    phazite::undo (*song);
    phazite::undo (*song);
    CHECK (phazite::getSongStanding (*song).versionsBase.isEmpty());
    CHECK (phazite::getSongStanding (*song).versionsBranch.isEmpty());
    CHECK (phazite::getLastKey (*song) == phazite::Key { .root = 5 });
    phazite::redo (*song);
    CHECK (phazite::getSongStanding (*song).versionsBase == base);
    CHECK (phazite::getSongStanding (*song).versionsBranch == firstBranch);
    CHECK (phazite::getLastKey (*song) == phazite::Key { .root = 5 });
    phazite::redo (*song);
    phazite::test::checkSong (*song);

    // A version applied's way: the key in its step. Written outside the history while the step is open (ValueArea.h: keyed),
    // the versions' branch, an entry the step didn't change, is taken into both its values, surviving its Undo and Redo; the
    // key, an entry the step changed, keeps the step's before and after, which its Undo and Redo put back, as the tree's Undo
    // and Redo of a property did.
    const auto branch = repeat ('b', schema::versionsBranchDigits);

    {
        const phazite::UndoStep step (*song, "Apply");
        auto next = phazite::getSongStanding (*song);
        next.lastKey = phazite::SavedKey { .root = 6, .scale = {} };
        phazite::setSongStanding (*song, std::move (next));

        auto names = phazite::getSongStanding (*song);
        names.versionsBranch = branch;
        phazite::storeStandingOutsideHistory (*song, std::move (names));
        phazite::setLastKey (*song, { .root = 9 });
        CHECK (phazite::getLastKey (*song) == phazite::Key { .root = 9 });
    }

    phazite::undo (*song);
    CHECK (phazite::getLastKey (*song) == phazite::Key { .root = 5 });
    CHECK (phazite::getSongStanding (*song).versionsBranch == branch);
    CHECK (phazite::getSongStanding (*song).versionsBase == base);
    phazite::redo (*song);
    CHECK (phazite::getLastKey (*song) == phazite::Key { .root = 6 });
    CHECK (phazite::getSongStanding (*song).versionsBranch == branch);
    phazite::test::checkSong (*song);
}

//==============================================================================
TEST_CASE ("Song's own areas: each writer of the song's own in the history is one exact undo step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    phazite::setLastKey (song, { .root = 1 });
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Change Notes", [&] { phazite::setProjectNotes (s.context, "Bridge in D"); });
    undo.check ("Outline",
                [&]
                {
                    const phazite::UndoStep step (song, "Outline");
                    phazite::test::setStepProbe (song, "verse-chorus");
                });
    undo.check ("Patterns",
                [&]
                {
                    const phazite::UndoStep step (song, "Patterns");
                    phazite::togglePatternsFollowTempo (song);
                });

    // A version applied that changes the key set last (which the check leaves out, as what the project remembers outside the
    // history: UndoCheck.cpp): its Undo and Redo checked here, exact.
    auto other = phazite::test::copyWholeSong (song);
    juce::ValueTree (other.getChildWithName (schema::LASTKEY)).setProperty (schema::keyRoot, 8, nullptr);
    other.setProperty (schema::notes, "Theirs", nullptr);
    undo.check ("Apply",
                [&]
                {
                    const phazite::UndoStep step (song, "Apply");
                    phazite::makeSongLike (song, other);
                });

    CHECK (phazite::getLastKey (song) == phazite::Key { .root = 8 });
    phazite::undo (song);
    CHECK (phazite::getLastKey (song) == phazite::Key { .root = 1 });
    CHECK (phazite::getProjectNotes (s.context) == "Bridge in D");
    phazite::redo (song);
    CHECK (phazite::getLastKey (song) == phazite::Key { .root = 8 });
    CHECK (phazite::getProjectNotes (s.context) == "Theirs");
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song's own areas: a merge taken and cancelled are each one exact undo step, the merge waiting the song's own")
{
    phazite::test::TestSong s;
    phazite::setProjectNotes (s.context, "Before");
    keepVersion (s.context, "First");
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    phazite::setProjectNotes (s.context, "Theirs");
    const auto theirs = keepVersion (s.context, "Theirs");
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    phazite::setProjectNotes (s.context, "Ours");
    keepVersion (s.context, "Ours");
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));

    auto& song = *phazite::findSong (s.context);
    phazite::test::UndoCheck undo (s.context);
    undo.check ("Merge", [&] { REQUIRE (phazite::test::finished (s.context, [&] { return phazite::finishMerge (s.context); })); });
    REQUIRE (phazite::isMerging (s.context));
    CHECK (phazite::getSongOwn (song).merging.has_value());
    phazite::test::checkSong (song);

    undo.check ("Cancel Merge", [&] { phazite::cancelMerge (s.context); });
    CHECK_FALSE (phazite::isMerging (s.context));
    CHECK_FALSE (phazite::getSongOwn (song).merging.has_value());
    phazite::test::checkSong (song);
}

//==============================================================================
TEST_CASE ("Song's own areas: what Phazite doesn't know at the song's root goes as the song opens, and its file is saved without it")
{
    phazite::test::TestSong s;
    phazite::setProjectNotes (s.context, "Kept");
    REQUIRE (s.getProject().save().wasOk());
    const auto file = s.getProject().getFile();

    // A later build's file: a property of the root's, and an element with something in it, of names Phazite doesn't know.
    auto xml = juce::parseXML (file);
    REQUIRE (xml != nullptr);
    xml->setAttribute ("laterProperty", "1");
    xml->createNewChildElement ("FUTURE")->createNewChildElement ("PART")->setAttribute ("x", "2");
    REQUIRE (xml->writeTo (file));

    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    auto& song = *phazite::findSong (s.context);
    const auto whole = phazite::test::copyWholeSong (song);
    CHECK_FALSE (whole.hasProperty ("laterProperty"));
    CHECK_FALSE (whole.getChildWithName ("FUTURE").isValid());
    CHECK (phazite::getProjectNotes (s.context) == "Kept");
    phazite::test::checkSong (song);

    // Saved: without them.
    phazite::setProjectNotes (s.context, "Saved again");
    REQUIRE (s.getProject().save().wasOk());
    const auto saved = juce::parseXML (file);
    REQUIRE (saved != nullptr);
    CHECK_FALSE (saved->hasAttribute ("laterProperty"));
    CHECK (saved->getChildByName ("FUTURE") == nullptr);
    CHECK (saved->getStringAttribute ("notes") == "Saved again");
}

TEST_CASE ("Song's own areas: how the song plays and shows, changed after a save, leaves the project saved; opened again, the song "
           "is the file's")
{
    phazite::test::TestSong s;
    auto& project = s.getProject();
    REQUIRE (project.save().wasOk());

    {
        auto& song = *phazite::findSong (s.context);
        phazite::toggleLoop (song);
        auto grid = phazite::loadGridSettings (song, phazite::GridOwner::playlist);
        grid.snap = ! grid.snap;
        phazite::saveGridSettings (song, phazite::GridOwner::playlist, grid);
        phazite::setPlayStart (song, at (8.0));
        CHECK_FALSE (project.hasUnsavedChanges());
        CHECK (project.isWholeAsSaved());
    }

    // Opened again without saving: as the file holds it, saved.
    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    auto& song = *phazite::findSong (s.context);
    CHECK_FALSE (phazite::isLoopOn (song));
    CHECK (phazite::getPlayStartBeat (song) == at (0.0));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    // Changed and saved: the file holds them, and the song opened again from it (a project opens at its start).
    phazite::toggleLoop (song);
    phazite::setPlayStart (song, at (8.0));
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    auto& reopened = *phazite::findSong (s.context);
    CHECK (phazite::isLoopOn (reopened));
    CHECK (phazite::getPlayStartBeat (reopened) == at (0.0));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    phazite::test::checkSong (reopened);
}

TEST_CASE ("Song's own areas: a version whose song's root holds what Phazite doesn't know, switched to, lists no change, leaves the "
           "project saved, and a save writes none of it")
{
    phazite::test::TestSong s;
    phazite::setProjectNotes (s.context, "Mine");
    keepVersion (s.context, "Mine");

    // A later build's commit (another computer's, on a branch of its own after this one's last): its song's root holds a
    // property and an element with something in it, of names Phazite doesn't know, and notes of its own.
    auto later = phazite::test::copyWholeSong (*phazite::findSong (s.context));
    later.removeProperty (schema::versionsBranch, nullptr); // where a song stands is no version's
    later.removeProperty (schema::versionsBase, nullptr);
    later.setProperty ("laterProperty", "1", nullptr);
    later.setProperty (schema::notes, "Later", nullptr);
    juce::ValueTree future ("FUTURE");
    future.appendChild (juce::ValueTree ("PART", { { "x", 2 } }), nullptr);
    later.appendChild (future, nullptr);

    {
        phazite::VersionStore other (s.getProject().getFolder().getChildFile ("Versions"), "another-computer");
        phazite::VersionRecord record;
        record.name = "From a later build";
        record.parents = { phazite::getLastVersion (s.context)->id };
        record.branch = "branch-later";
        record.saved = juce::Time::currentTimeMillis();
        const auto id = other.writeVersion (record, phazite::splitSong (later));
        REQUIRE (id.isNotEmpty());

        phazite::BranchRecord branch;
        branch.id = record.branch;
        branch.name = "later";
        branch.colour = juce::Colours::orange;
        branch.head = id;
        branch.computer = "another-computer";
        branch.changed = record.saved;
        REQUIRE (other.writeBranch (branch));
        other.getPieces().closeFiles();
    }

    s.reopen (false);
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "later"), s.save); }));

    // Switched to: the version's notes, none of what Phazite doesn't know, and no change listed against the version.
    auto& song = *phazite::findSong (s.context);
    CHECK (phazite::getProjectNotes (s.context) == "Later");
    CHECK_FALSE (phazite::test::copyWholeSong (song).hasProperty ("laterProperty"));
    CHECK_FALSE (phazite::test::copyWholeSong (song).getChildWithName ("FUTURE").isValid());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (s.context)));
    CHECK (phazite::getChangesSinceLastVersion (s.context).empty());
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    phazite::test::checkSong (song);

    // Saved: none of it in the file.
    REQUIRE (s.getProject().save().wasOk());
    const auto saved = juce::parseXML (s.getProject().getFile());
    REQUIRE (saved != nullptr);
    CHECK_FALSE (saved->hasAttribute ("laterProperty"));
    CHECK (saved->getChildByName ("FUTURE") == nullptr);
    CHECK (saved->getStringAttribute ("notes") == "Later");
}

//==============================================================================
TEST_CASE ("Song's own areas, with the tracks': the count of IDs is past a track's fader, the largest ID the song holds, which only "
           "the tracks' area holds")
{
    const auto song = phazite::createSong();
    const auto track = phazite::addTrackAtEnd (*song, false, nullptr);
    const auto held = phazite::findHeldTrack (*song, track);
    REQUIRE (held.has_value());
    const auto fader = held->fader.getRaw();
    REQUIRE (fader > track.getRaw()); // made after the track's own: the largest ID the song holds
    REQUIRE (phazite::getNextID (*song) == fader + 1);

    // The count put back at the fader (as a copy's may be: IDCount::mayBeBehind): what the song finds its next ID to be is past
    // the fader all the same, and the check reports the count, both counting the tracks' area.
    phazite::countIDsFrom (*song, fader, phazite::IDCount::mayBeBehind);
    CHECK (phazite::getNextID (*song) == fader);
    CHECK (phazite::findNextID (*song) == fader + 1);
    const auto problems = phazite::test::findValueProblems (*song);
    CHECK (std::any_of (problems.begin(), problems.end(),
                        [] (const std::string& problem) { return problem.find ("count of IDs") != std::string::npos; }));

    // Counted again: past the fader, in the song and in its whole copy, and the next ID made is past it.
    phazite::countIDs (*song);
    CHECK (phazite::getNextID (*song) == fader + 1);
    CHECK (phazite::readID (phazite::test::copyWholeSong (*song)[schema::nextID]) == fader + 1);
    phazite::test::checkSong (*song);
    CHECK (song->makeID() == fader + 1);
}

TEST_CASE ("Song's own areas, with the tracks': a project opened is the same song "
           "saved and opened again, and its count of IDs never goes back across an Undo and a Redo")
{
    phazite::test::TestSong s;
    const auto lead = s.addTrack ("Lead");
    const auto other = s.addTrack ("Other");
    s.addBus ("Folder", { other });
    s.setInstrument (lead, phazite::devices::Synth::typeID);
    s.addEffect (lead, "phaziteDelay");
    s.addNote (s.addMidiClip (lead, 0.0, 4.0), 60, 0.0, 1.0);

    {
        auto& song = *phazite::findSong (s.context);
        phazite::setLastKey (song, { .root = 3, .scale = phazite::Scale::minor });
        phazite::toggleLoop (song);

        // The merge waiting set by hand, as a song just opened holds one: in a song with nothing to undo (setWaitingMerge()).
        song.getUndoManager().clearUndoHistory();
        phazite::setWaitingMerge (
            song, phazite::writeMergingElement ({ .version = repeat ('a', 64), .head = repeat ('b', 64), .before = repeat ('c', 64) }));
    }

    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    auto* song = phazite::findSong (s.context);
    CHECK (phazite::isLoopOn (*song));
    CHECK (phazite::getLastKey (*song) == phazite::Key { .root = 3, .scale = phazite::Scale::minor });
    CHECK (phazite::getSongOwn (*song).merging.has_value());
    CHECK (phazite::findHeldTrack (*song, lead).has_value());
    REQUIRE (phazite::findHeldTrack (*song, other).has_value());
    CHECK (phazite::isFolder (*song, phazite::findHeldTrack (*song, other)->parent)); // in its folder
    phazite::test::checkSong (*song);

    // Saved and opened again: the same song, whole.
    const auto opened = phazite::test::copyWholeSong (*song);
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.reopenAs (phazite::test::TestSong::Reopening::withoutSaving));
    song = phazite::findSong (s.context);
    CHECK (phazite::test::copyWholeSong (*song).isEquivalentTo (opened));
    phazite::test::checkSong (*song);

    // A track added (IDs made), its Undo and its Redo: the count never goes back, and the track's IDs stay below it.
    const auto before = phazite::getNextID (*song);
    phazite::TrackID added;

    {
        const phazite::UndoStep step (*song, "Add Track");
        added = phazite::addTrackAtEnd (*song, false, nullptr);
    }

    const auto held = phazite::findHeldTrack (*song, added);
    REQUIRE (held.has_value());
    const auto after = phazite::getNextID (*song);
    CHECK (after > before);
    CHECK (added.getRaw() < after);
    CHECK (held->fader.getRaw() < after);
    phazite::test::checkSong (*song);

    phazite::undo (*song);
    CHECK_FALSE (phazite::findHeldTrack (*song, added).has_value());
    CHECK (phazite::getNextID (*song) == after);
    phazite::test::checkSong (*song);

    phazite::redo (*song);
    REQUIRE (phazite::findHeldTrack (*song, added).has_value());
    CHECK (phazite::getNextID (*song) == after);
    CHECK (phazite::findHeldTrack (*song, added)->fader == held->fader);
    phazite::test::checkSong (*song);

    // The next ID made is past them all.
    CHECK (song->makeID() == after);
}
