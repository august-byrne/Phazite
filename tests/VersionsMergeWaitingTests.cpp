// SPDX-License-Identifier: AGPL-3.0-or-later
// A merge taken into the song that waits for its commit (docs/design/versions.md: Merging; git's MERGE_HEAD): kept in the song
// (Schema.h: MERGING), so that whichever file of the song opens (the project file saved or not, its autosave after a crash)
// holds the merge state its song was in; waiting on top of a branch that moved on, and no more where the branch holds the
// merged commit already; never two at once; and restores from a damaged commit that can't tell where a line was.
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name = {})
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
    return phazite::getLastVersion (context)->id;
}

std::vector<phazite::RenumberedID> sorted (std::vector<phazite::RenumberedID> ids)
{
    std::sort (ids.begin(), ids.end(),
               [] (const auto& a, const auto& b) { return a.clip != b.clip ? a.clip.compare (b.clip) < 0 : a.from < b.from; });
    return ids;
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

// A song with Bass and Drums committed on main; idea-2 turned Bass down to -12 dB and added Pad, main to -6 dB and added Lead,
// each counting its IDs on from the same place (as two computers would), so that the merge gives Pad a new ID: a conflict,
// an ID given anew, and main's song, saved, before the merge.
struct WaitingMerge
{
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass"), drums = s.addTrack ("Drums");
    juce::String first = keepVersion (s.context, "First"), theirs, ours;
    phazite::TrackID pad, lead;

    WaitingMerge()
    {
        auto& song = *phazite::findSong (s.context);
        const auto count = song.getState()[schema::nextID];
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        s.setVolume (bass, -12.0f);
        pad = s.addTrack ("Pad");
        theirs = keepVersion (s.context, "Much quieter");

        REQUIRE (
            phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
        song.getState().setProperty (schema::nextID, count, nullptr);
        s.setVolume (bass, -6.0f);
        lead = s.addTrack ("Lead");
        REQUIRE (lead == pad);
        ours = keepVersion (s.context, "Quieter");
        REQUIRE (s.getProject().save().wasOk());
    }

    phazite::Song& song() { return *phazite::findSong (s.context); }
    float volume (phazite::TrackID track) { return phazite::getChannelVolume (song(), track); }

    // The merge taken, and the IDs it gave anew (Pad's, and its fader's), which its commit must record.
    std::vector<phazite::RenumberedID> given;

    void merge()
    {
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
        phazite::finishMerge (s.context);
        REQUIRE (phazite::isMerging (s.context));
        REQUIRE (phazite::getClashes (s.context).size() == 1);
        given = phazite::getVersionHistory (s.context)->getMerging()->renumbered;
        REQUIRE_FALSE (given.empty());
    }

    std::vector<phazite::TrackID> named (const juce::String& name)
    {
        std::vector<phazite::TrackID> found;

        for (const auto track : phazite::getTrackIDs (song()))
            if (phazite::getTrackName (song(), track) == name)
                found.push_back (track);

        return found;
    }

    const phazite::VersionRecord& lastCommit()
    {
        const auto* last = phazite::getVersionHistory (s.context)->findLastVersion();
        REQUIRE (last != nullptr);
        return *last;
    }

    void reopenAs (TestSong::Reopening how)
    {
        REQUIRE (waitForVersionHistory (s.context));
        REQUIRE (s.reopenAs (how));
        REQUIRE (waitForVersionHistory (s.context));
    }
};

// A commit another computer made on a branch, on top of one, with its song and its parents, and that computer's record
// of the branch at it: as a synced Versions folder brings them.
juce::String commitElsewhere (TestSong& s, const juce::String& branchID, juce::StringArray parents,
                              std::vector<phazite::RenumberedID> renumbered = {})
{
    const auto folder = phazite::getVersionsFolder (*phazite::findSong (s.context));
    phazite::VersionStore store (folder, "elsewhere");
    phazite::VersionRecord record;
    record.name = "From elsewhere";
    record.parents = parents;
    record.renumbered = std::move (renumbered);
    record.branch = branchID;
    record.saved = juce::Time::currentTimeMillis();
    const auto song = phazite::getVersionHistory (s.context)->readSong (parents[0]);
    REQUIRE (song.isValid());
    const auto id = store.writeVersion (record, phazite::splitSong (song));
    REQUIRE (id.isNotEmpty());

    phazite::BranchRecord branch;

    for (const auto& b : phazite::getVersionHistory (s.context)->getBranches())
        if (b.id == branchID)
            branch = b;

    branch.head = id;
    branch.computer = "elsewhere";
    branch.working = {};
    REQUIRE (store.writeBranch (branch));
    store.getPieces().closeFiles();
    return id;
}

juce::String commitElsewhere (WaitingMerge& m, const juce::String& branchID, juce::StringArray parents,
                              std::vector<phazite::RenumberedID> renumbered = {})
{
    return commitElsewhere (m.s, branchID, std::move (parents), std::move (renumbered));
}
} // namespace

TEST_CASE ("A merge taken after the last save goes with the song closed without saving; one saved with the song waits as it opens")
{
    // Closed with Don't Save: the song as saved, from before the merge, and no merge waits; the next commit follows this line
    // alone, and the other line can be merged again, its work arriving.
    {
        WaitingMerge m;
        m.merge();
        m.reopenAs (TestSong::Reopening::withoutSaving);
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (m.volume (m.bass) == doctest::Approx (-6.0f));
        CHECK (m.named ("Pad").empty());
        CHECK_FALSE (phazite::hasSomethingToCommit (withChangesIn (m.s.context)));

        m.s.setVolume (m.drums, -3.0f);
        keepVersion (m.s.context, "Drums a little down");
        CHECK (m.lastCommit().parents.size() == 1);
        REQUIRE (
            phazite::test::finished (m.s.context, [&] { return phazite::mergeBranch (m.s.context, findBranch (m.s.context, "idea-2")); }));
        phazite::finishMerge (m.s.context);
        CHECK (m.named ("Pad").size() == 1);
    }

    // Merged, saved, Undone, closed with Don't Save: the saved song holds the merge, and so it waits, its commit following
    // both lines with the ID it gave Pad recorded; a later merge of that line finds Pad again rather than adding it twice.
    {
        WaitingMerge m;
        m.merge();
        const auto padHere = m.named ("Pad");
        REQUIRE (padHere.size() == 1);
        REQUIRE (m.s.getProject().save().wasOk());
        phazite::undo (m.song());
        REQUIRE_FALSE (phazite::isMerging (m.s.context));
        m.reopenAs (TestSong::Reopening::withoutSaving);
        REQUIRE (phazite::isMerging (m.s.context));
        CHECK (m.named ("Pad") == padHere);
        CHECK (phazite::getClashes (m.s.context).size() == 1);

        keepVersion (m.s.context);
        CHECK (m.lastCommit().parents == juce::StringArray (m.ours, m.theirs));
        CHECK (sorted (m.lastCommit().renumbered) == sorted (m.given));
        CHECK (std::find (m.given.begin(), m.given.end(), phazite::RenumberedID { {}, m.pad.getRaw(), padHere[0].getRaw() })
               != m.given.end());

        REQUIRE (phazite::test::finished (m.s.context, [&]
                                          { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "idea-2"), m.s.save); }));
        m.s.rename (m.pad, "Pad 2");
        const auto again = keepVersion (m.s.context, "Pad 2");
        REQUIRE (phazite::test::finished (m.s.context,
                                          [&] { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "main"), m.s.save); }));
        const auto numTracks = phazite::getTrackIDs (m.song()).size();
        REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, again); }));
        phazite::finishMerge (m.s.context);
        checkIDs (m.song());
        CHECK (m.named ("Pad 2") == padHere);
        CHECK (phazite::getTrackIDs (m.song()).size() == numTracks);
    }
}

TEST_CASE ("After a crash, the autosave restored holds the merge that waited, and the last saved version opened holds none")
{
    {
        WaitingMerge m;
        m.merge();
        REQUIRE (phazite::chooseClash (m.s.context, 0, 2));
        m.reopenAs (TestSong::Reopening::afterCrashRestoring);
        REQUIRE (phazite::isMerging (m.s.context));
        CHECK (phazite::getClashes (m.s.context)[0].chosen == 2);
        CHECK (m.volume (m.bass) == doctest::Approx (-12.0f));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
    }

    {
        WaitingMerge m;
        m.merge();
        m.reopenAs (TestSong::Reopening::afterCrashFromFile);
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (m.named ("Pad").empty());
    }
}

TEST_CASE ("A merge taken counts as an unsaved change, and the merge itself is no line of the changes")
{
    WaitingMerge m;
    REQUIRE_FALSE (m.s.getProject().hasUnsavedChanges());
    REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, m.theirs); }));
    phazite::finishMerge (m.s.context);
    REQUIRE (phazite::isMerging (m.s.context));
    CHECK (m.s.getProject().hasUnsavedChanges());

    // What it brought (Bass and Pad) are lines; its element in the song (Schema.h: MERGING) is none, not even the song's own.
    const auto lines = phazite::getChangesSinceLastVersion (withChangesIn (m.s.context));
    CHECK_FALSE (lines.empty());
    CHECK (std::none_of (lines.begin(), lines.end(), [] (const phazite::VersionChange& line) { return line.isSong; }));

    // Each line discarded, the song holds what it was saved with but the merge waiting: that alone is an unsaved change, and
    // something to save a version of.
    for (const auto& line : lines)
        REQUIRE (phazite::takeBack (m.s.context, line));

    REQUIRE (dispatchMessagesUntil ([&m] { return ! phazite::hasChangesSinceLastVersion (withChangesIn (m.s.context)); }));
    REQUIRE (phazite::isMerging (m.s.context));
    CHECK (m.s.getProject().hasUnsavedChanges());
    CHECK (phazite::hasSomethingToCommit (withChangesIn (m.s.context)));
}

TEST_CASE ("A merge redone after its Undo and a commit waits on top of that commit, and its commit follows it and the merged one")
{
    WaitingMerge m;
    m.s.setVolume (m.drums, -3.0f); // a change of this line's, to commit after the Undo
    m.merge();
    phazite::undo (m.song());
    REQUIRE_FALSE (phazite::isMerging (m.s.context));
    const auto between = keepVersion (m.s.context, "Drums a little down");
    REQUIRE (m.lastCommit().parents.size() == 1);

    phazite::redo (m.song());
    REQUIRE (phazite::isMerging (m.s.context));
    CHECK (phazite::getMergeProblem (m.s.context).contains ("moved on"));
    CHECK (m.named ("Pad").size() == 1);
    keepVersion (m.s.context);
    CHECK (m.lastCommit().parents == juce::StringArray (between, m.theirs));
    CHECK (sorted (m.lastCommit().renumbered) == sorted (m.given));
    CHECK_FALSE (phazite::isMerging (m.s.context));
    CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
}

TEST_CASE ("A merge waiting while another computer commits on the branch goes its own way beside that commit; one it merged there "
           "waits no more")
{
    // Another computer's commit on main, on top of the one the merge was taken at, which the song didn't grow from: the view
    // says the branch has a newer version; the merge's version follows the song's line and the one merged in, never that
    // commit, whose work the song hasn't, and the branch shows the two lines to merge again (S4.5).
    {
        WaitingMerge m;
        m.merge();
        REQUIRE (m.s.getProject().save().wasOk());
        const auto elsewhere = commitElsewhere (m, findBranch (m.s.context, "main"), { m.ours });
        m.reopenAs (TestSong::Reopening::withoutSaving);
        REQUIRE (m.lastCommit().id == elsewhere);
        REQUIRE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
        CHECK (phazite::countNewerVersions (m.s.context) == 1);
        CHECK (phazite::canSaveVersion (withChangesIn (m.s.context)));
        keepVersion (m.s.context);
        CHECK (m.lastCommit().parents == juce::StringArray (m.ours, m.theirs));
        CHECK (sorted (m.lastCommit().renumbered) == sorted (m.given));
        CHECK (phazite::getDivergentVersions (m.s.context) == juce::StringArray (elsewhere));
    }

    // Another computer merged the same commit on main, giving the same IDs: that merge is as good as this one's, and none
    // waits here, without a word.
    {
        WaitingMerge m;
        m.merge();
        REQUIRE (m.s.getProject().save().wasOk());
        commitElsewhere (m, findBranch (m.s.context, "main"), { m.ours, m.theirs }, m.given);
        m.reopenAs (TestSong::Reopening::withoutSaving);
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
    }

    // The branch holds the commit merged in by no merge of it (the other line taken in whole, then this one): none waits,
    // and the view says why, and that Cancel Merge takes the element out.
    {
        WaitingMerge m;
        m.merge();
        REQUIRE (m.s.getProject().save().wasOk());
        commitElsewhere (m, findBranch (m.s.context, "main"), { m.theirs, m.ours });
        m.reopenAs (TestSong::Reopening::withoutSaving);
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).contains ("already"));
        CHECK (phazite::getMergeProblem (m.s.context).contains ("Cancel Merge"));
        REQUIRE (phazite::canCancelMerge (m.s.context));
        phazite::cancelMerge (m.s.context);
        CHECK_FALSE (m.song().getState().getChildWithName (schema::MERGING).isValid());
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
    }
}

TEST_CASE ("Another computer's merge of the same commit with other IDs leaves this one waiting, and later merges of that line "
           "find this song's items")
{
    // Computer A (this one) merged idea-2's Pad as one ID; computer B merged it too, as another, and committed. A commits its
    // merge as a line of its own beside B's (the song didn't grow from B's), recording its own IDs; then idea-2 goes on, Pad
    // renamed, or deleted, and A merges it again: Pad once, renamed, or gone.
    for (const auto deletes : { false, true })
    {
        INFO ((deletes ? "deleted" : "renamed"));
        WaitingMerge m;
        m.merge();
        const auto padHere = m.named ("Pad");
        REQUIRE (padHere.size() == 1);
        REQUIRE (m.s.getProject().save().wasOk());

        const auto byB = commitElsewhere (m, findBranch (m.s.context, "main"), { m.ours, m.theirs },
                                          { { {}, m.pad.getRaw(), padHere[0].getRaw() + 1000 } });
        m.reopenAs (TestSong::Reopening::withoutSaving);
        REQUIRE (m.lastCommit().id == byB);
        REQUIRE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).contains ("already"));
        keepVersion (m.s.context);
        CHECK (m.lastCommit().parents == juce::StringArray (m.ours, m.theirs));
        CHECK (phazite::getDivergentVersions (m.s.context) == juce::StringArray (byB));
        CHECK (sorted (m.lastCommit().renumbered) == sorted (m.given));
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());

        REQUIRE (phazite::test::finished (m.s.context, [&]
                                          { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "idea-2"), m.s.save); }));

        if (deletes)
        {
            const phazite::UndoStep step (m.song(), "Delete Track");
            phazite::deleteTracks (m.song(), { m.pad }, nullptr);
        }
        else
        {
            m.s.rename (m.pad, "Pad 2");
        }

        const auto later = keepVersion (m.s.context, "Pad again");
        REQUIRE (phazite::test::finished (m.s.context,
                                          [&] { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "main"), m.s.save); }));
        REQUIRE (waitForVersionHistory (m.s.context));
        const auto numTracks = phazite::getTrackIDs (m.song()).size();
        REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, later); }));
        phazite::finishMerge (m.s.context);
        checkIDs (m.song());
        CHECK (m.named ("Pad").empty());

        if (deletes)
        {
            CHECK (m.named ("Pad 2").empty());
            CHECK (phazite::getTrackIDs (m.song()).size() == numTracks - 1);
        }
        else
        {
            CHECK (m.named ("Pad 2") == padHere);
            CHECK (phazite::getTrackIDs (m.song()).size() == numTracks);
        }
    }
}

TEST_CASE ("A merge waiting on a branch left for another waits there still after the project opens again")
{
    WaitingMerge m;
    m.merge();
    REQUIRE (phazite::chooseClash (m.s.context, 0, 2));

    // Off to idea-3, made from the first commit: main keeps the merge in the song left there, and the branch whose commit
    // it merges can't be deleted meanwhile.
    REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::newBranch (m.s.context, m.first, m.s.save); }));
    CHECK_FALSE (phazite::isMerging (m.s.context));
    CHECK_FALSE (phazite::canDeleteBranch (m.s.context, findBranch (m.s.context, "idea-2")));

    m.reopenAs (TestSong::Reopening::withoutSaving);
    CHECK_FALSE (phazite::isMerging (m.s.context));
    CHECK_FALSE (phazite::canDeleteBranch (m.s.context, findBranch (m.s.context, "idea-2")));

    REQUIRE (phazite::test::finished (m.s.context,
                                      [&] { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "main"), m.s.save); }));
    REQUIRE (phazite::isMerging (m.s.context)); // being read, as the song left there holds it
    REQUIRE (waitForVersionHistory (m.s.context));
    REQUIRE (phazite::isMerging (m.s.context));
    CHECK (phazite::getClashes (m.s.context)[0].chosen == 2);
    CHECK (m.volume (m.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("No merge starts while one waits for its commit, as git refuses one: commit it or abort it first")
{
    WaitingMerge m;
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, m.s.context, m.s.save, m.s.renders);
    m.merge();

    phazite::selectVersion (m.s.context, m.theirs);
    CHECK_FALSE (phazite::canMerge (m.s.context));
    CHECK_FALSE (commands.find ("version.merge")->isEnabled());
    CHECK_FALSE (phazite::canMergeBranch (m.s.context, findBranch (m.s.context, "idea-2")));
    CHECK_FALSE (phazite::canMergeAnyBranch (m.s.context));
    CHECK_FALSE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, m.theirs); }));
    CHECK (phazite::isMerging (m.s.context));

    phazite::cancelMerge (m.s.context);
    CHECK_FALSE (phazite::isMerging (m.s.context));
    phazite::selectVersion (m.s.context, m.theirs);
    CHECK (phazite::canMerge (m.s.context));
}

TEST_CASE ("A merge's element that isn't one (a song from anyone) waits for nothing, and Cancel Merge never takes a track for the song")
{
    WaitingMerge m;
    m.merge();
    auto element = m.song().getState().getChildWithName (schema::MERGING);
    REQUIRE (element.isValid());

    // Its song before it a track's piece: still waiting (the commit it merged is known), but Cancel Merge makes the song the
    // branch's last commit, not a track.
    juce::String trackPiece;

    for (const auto& [name, piece] : phazite::splitSong (m.song().getState()).pieces)
        if (piece.toString().startsWith ("<" + schema::TRACK.toString()))
            trackPiece = name;

    REQUIRE (trackPiece.isNotEmpty());
    element.setProperty (schema::mergingBefore, trackPiece, nullptr);
    REQUIRE (m.s.getProject().save().wasOk());
    m.reopenAs (TestSong::Reopening::withoutSaving);
    REQUIRE (phazite::isMerging (m.s.context));
    CHECK (phazite::getMergeProblem (m.s.context).contains ("can't read"));
    phazite::cancelMerge (m.s.context);
    CHECK_FALSE (phazite::isMerging (m.s.context));
    CHECK (phazite::getTrackIDs (m.song()).size() >= 3);
    CHECK (m.named ("Pad").empty());

    // IDs given anew that aren't one to one, or a commit that isn't one: no merge waits, and the view says so.
    for (const auto& [property, value] : { std::pair { schema::mergingRenumbered, juce::String ("10>12,11>12") },
                                           std::pair { schema::mergingTranslated, juce::String ("0>4") },
                                           std::pair { schema::mergingVersion, juce::String ("not a commit") } })
    {
        WaitingMerge n;
        n.merge();
        n.song().getState().getChildWithName (schema::MERGING).setProperty (property, value, nullptr);
        REQUIRE (n.s.getProject().save().wasOk());
        n.reopenAs (TestSong::Reopening::withoutSaving);
        CHECK_FALSE (phazite::isMerging (n.s.context));
        CHECK (phazite::getMergeProblem (n.s.context).contains ("can't be read"));
        checkIDs (n.song());

        // Cancel Merge takes the element out, alone, as an undo step of its own.
        const auto numTracks = phazite::getTrackIDs (n.song()).size();
        REQUIRE (phazite::canCancelMerge (n.s.context));
        phazite::cancelMerge (n.s.context);
        CHECK_FALSE (n.song().getState().getChildWithName (schema::MERGING).isValid());
        CHECK (phazite::getTrackIDs (n.song()).size() == numTracks);
        CHECK (n.named ("Pad").size() == 1); // what the merge brought stays, as the rest of the song
        CHECK (phazite::getMergeProblem (n.s.context).isEmpty());
        CHECK_FALSE (phazite::canCancelMerge (n.s.context));
        phazite::undo (n.song());
        CHECK (n.song().getState().getChildWithName (schema::MERGING).isValid());
    }
}

TEST_CASE ("A merge commit made after a save leaves the project saved, and no word of it as it opens again or as Local History "
           "brings back the song it waited in")
{
    // Closed with Don't Save after the merge's commit (which saves the project first, as Commit does).
    {
        WaitingMerge m;
        m.merge();
        REQUIRE (m.s.getProject().save().wasOk());
        keepVersion (m.s.context);
        REQUIRE (m.lastCommit().parents.size() == 2);
        CHECK_FALSE (m.s.getProject().hasUnsavedChanges());
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
        CHECK_FALSE (phazite::canCancelMerge (m.s.context));
        m.reopenAs (TestSong::Reopening::withoutSaving);
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
    }

    // A snapshot taken while the merge waited, brought back once it is committed: it holds the merge's element, which its
    // commit finished.
    {
        WaitingMerge m;
        m.merge();
        phazite::getVersionHistory (m.s.context)->takeSnapshot ("auto");
        REQUIRE (waitForVersionHistory (m.s.context));
        const auto snapshots = phazite::getSnapshots (m.s.context);
        REQUIRE_FALSE (snapshots.empty());
        keepVersion (m.s.context);
        REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::restoreSnapshot (m.s.context, snapshots.front().time); }));
        REQUIRE (m.song().getState().getChildWithName (schema::MERGING).isValid());
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).isEmpty());
    }
}

TEST_CASE ("A project file opened without its Versions folder holds a merge of another history, which Cancel Merge takes out")
{
    // Kept as it was, rather than taken out as the project opens: the folder may still be on its way (a sync), and the
    // notice says what the element is.
    WaitingMerge m;
    m.merge();
    REQUIRE (m.s.getProject().save().wasOk());
    REQUIRE (waitForVersionHistory (m.s.context));

    TemporaryFolder elsewhere;
    const auto copy = elsewhere.folder.getChildFile (m.s.getProject().getFile().getFileName());
    REQUIRE (m.s.getProject().getFile().copyFileTo (copy));
    juce::String error;
    phazite::setSong (m.s.context, nullptr);
    auto project = phazite::Project::open (copy, false, error);
    REQUIRE (project != nullptr);
    phazite::setSong (m.s.context, &project->getSong());
    REQUIRE (waitForVersionHistory (m.s.context));
    auto& song = project->getSong();

    REQUIRE (song.getState().getChildWithName (schema::MERGING).isValid());
    CHECK_FALSE (phazite::isMerging (m.s.context));
    CHECK (phazite::getMergeProblem (m.s.context).contains ("Cancel Merge"));
    const auto numTracks = phazite::getTrackIDs (song).size();
    REQUIRE (phazite::canCancelMerge (m.s.context));
    phazite::cancelMerge (m.s.context);
    CHECK_FALSE (song.getState().getChildWithName (schema::MERGING).isValid());
    CHECK (phazite::getTrackIDs (song).size() == numTracks);
    CHECK (phazite::getMergeProblem (m.s.context).isEmpty());

    // Its first commit, as a new history makes it, holds none either.
    const phazite::VersionStore store (phazite::getVersionsFolder (song), "reader");
    REQUIRE (phazite::getLastVersion (m.s.context).has_value());
    const auto* first = phazite::getVersionHistory (m.s.context)->findLastVersion();
    REQUIRE (first != nullptr);
    CHECK_FALSE (store.readSong (first->song).getChildWithName (schema::MERGING).isValid());
    store.getPieces().closeFiles();
    phazite::setSong (m.s.context, nullptr);
}

TEST_CASE ("No version's song holds a merge's element: not one that waits for nothing, nor after a merge's own version")
{
    WaitingMerge m;
    m.merge();
    keepVersion (m.s.context); // the merge's own version, the element finished in the song

    // An element that waits for nothing, and a change, saved with the whole song.
    m.song().getState().getChildWithName (schema::MERGING).setProperty (schema::mergingVersion, "not a version", nullptr);
    m.s.setVolume (m.drums, -3.0f);
    keepVersion (m.s.context, "Drums down");

    const phazite::VersionStore store (phazite::getVersionsFolder (m.song()), "reader");

    for (const auto* version : phazite::getVersionHistory (m.s.context)->getVersions())
    {
        INFO (phazite::getDisplayName (*version).toStdString());
        CHECK_FALSE (store.readSong (version->song).getChildWithName (schema::MERGING).isValid());
    }

    store.getPieces().closeFiles();
}

TEST_CASE ("Cancel Merge keeps the song in Local History first, and makes it the song before the merge wherever that can be read")
{
    // Kept in Local History.
    {
        WaitingMerge m;
        m.merge();
        phazite::cancelMerge (m.s.context);
        REQUIRE (waitForVersionHistory (m.s.context));
        const auto snapshots = phazite::getSnapshots (m.s.context);
        CHECK (std::any_of (snapshots.begin(), snapshots.end(),
                            [] (const phazite::SnapshotInfo& snapshot) { return snapshot.reason == "Before Cancel Merge"; }));
    }

    // The commit it was taken at gone from the history (another computer's Undo Last Commit): its conflicts can't be found,
    // but Cancel Merge makes the song what it was before it, the change made before the merge with it.
    {
        WaitingMerge m;
        m.s.setVolume (m.drums, -3.0f);
        m.merge();
        m.song()
            .getState()
            .getChildWithName (schema::MERGING)
            .setProperty (schema::mergingHead, juce::String::repeatedString ("ab", 32), nullptr);
        REQUIRE (m.s.getProject().save().wasOk());
        m.reopenAs (TestSong::Reopening::withoutSaving);
        REQUIRE (phazite::isMerging (m.s.context));
        CHECK (phazite::getClashes (m.s.context).empty());
        CHECK (phazite::getMergeProblem (m.s.context).contains ("what it was before it"));
        REQUIRE (phazite::canCancelMerge (m.s.context));
        phazite::cancelMerge (m.s.context);
        CHECK_FALSE (phazite::isMerging (m.s.context));
        CHECK (m.named ("Pad").empty());
        CHECK (m.volume (m.drums) == doctest::Approx (-3.0f));
        CHECK (m.volume (m.bass) == doctest::Approx (-6.0f));
    }
}

TEST_CASE ("A merge the store can't keep the song before takes nothing, its list stays, and the view says why")
{
    WaitingMerge m;
    m.s.setVolume (m.drums, -3.0f); // so that the song before the merge has a piece the store hasn't
    REQUIRE (waitForVersionHistory (m.s.context));

    // Every pack written into the project's store fails meanwhile, as a full or locked drive makes it.
    std::atomic<bool> blocked { true };
    const auto pieces = phazite::getVersionsFolder (m.song()).getChildFile ("pieces");
    const phazite::WholeFileHook hook ([&blocked, pieces] (const juce::File& file)
                                       { return ! (blocked.load() && file.isAChildOf (pieces) && file.hasFileExtension ("pack")); });

    // Kept as the merge's list is worked out, on the history's thread (known bug 126); that failing, kept again as Merge is
    // clicked, failing too.
    REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, m.theirs); }));
    CHECK_FALSE (phazite::test::finished (m.s.context,
                                          [&]
                                          {
                                              phazite::finishMerge (m.s.context);
                                              return true;
                                          }));
    CHECK_FALSE (phazite::isMerging (m.s.context));
    CHECK_FALSE (phazite::getMergeLines (m.s.context).empty());
    CHECK (phazite::getMergeProblem (m.s.context).contains ("Nothing was merged"));
    CHECK (m.named ("Pad").empty());

    blocked = false;
    REQUIRE (phazite::test::finished (m.s.context,
                                      [&]
                                      {
                                          phazite::finishMerge (m.s.context);
                                          return true;
                                      }));
    CHECK (phazite::isMerging (m.s.context));
    CHECK (phazite::getMergeLines (m.s.context).empty());
    CHECK_FALSE (phazite::getMergeProblem (m.s.context).contains ("Nothing was merged"));
}

TEST_CASE ("A branch whose line alone holds the commit a merge waiting took in can't be deleted, though it moved on since")
{
    WaitingMerge m;
    REQUIRE (phazite::test::finished (m.s.context,
                                      [&] { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "idea-2"), m.s.save); }));
    m.s.setVolume (m.drums, -9.0f);
    keepVersion (m.s.context, "Moved on");
    REQUIRE (phazite::test::finished (m.s.context,
                                      [&] { return phazite::switchBranch (m.s.context, findBranch (m.s.context, "main"), m.s.save); }));

    REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, m.theirs); })); // idea-2's older commit
    phazite::finishMerge (m.s.context);
    REQUIRE (phazite::isMerging (m.s.context));
    const auto idea = findBranch (m.s.context, "idea-2");
    CHECK_FALSE (phazite::canDeleteBranch (m.s.context, idea));
    CHECK_FALSE (phazite::deleteBranch (m.s.context, idea));

    keepVersion (m.s.context); // committed: main's line holds it now
    CHECK (phazite::canDeleteBranch (m.s.context, idea));
}

namespace
{
// A commit of a song written into the history by another computer, with what it holds of it lost (the pieces of the
// elements given), and made a branch of, as Recover Lost Versions makes one: its id.
juce::String commitDamaged (TestSong& s, const juce::ValueTree& song, const std::function<bool (const juce::ValueTree&)>& lost)
{
    auto pieces = phazite::splitSong (song);

    for (auto it = pieces.pieces.begin(); it != pieces.pieces.end();)
        it = lost (phazite::readElement (it->second)) ? pieces.pieces.erase (it) : std::next (it);

    juce::String id;

    {
        phazite::VersionStore store (phazite::getVersionsFolder (*phazite::findSong (s.context)), "elsewhere");
        phazite::VersionRecord record;
        record.name = "Damaged";
        record.parents.add (phazite::getLastVersion (s.context)->id);
        record.branch = phazite::getCurrentBranch (s.context)->id;
        record.saved = juce::Time::currentTimeMillis();
        id = store.writeVersion (record, pieces);
        store.getPieces().closeFiles();
    }

    REQUIRE (id.isNotEmpty());
    REQUIRE (phazite::verifyHistory (s.context));
    REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isVerifyingHistory (s.context); }, versionHistoryTimeout));
    const auto found = phazite::getLostVersions (s.context);
    REQUIRE (found.size() == 1);
    REQUIRE (phazite::recoverLostVersions (s.context, found[0].key).isNotEmpty());
    REQUIRE (phazite::getVersionHistory (s.context)->findVersion (id) != nullptr);
    return id;
}
} // namespace

TEST_CASE ("Restoring a line from a damaged commit takes what it can, and nothing where the line's place in it can't be read")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto bass = s.addTrack ("Bass"), keys = s.addTrack ("Keys");
    phazite::TrackID folder;

    {
        const phazite::UndoStep step (song, "Group");
        folder = phazite::groupIntoFolder (song, { bass }, nullptr);
        phazite::setTrackName (song, folder, "Low end");
    }

    keepVersion (s.context, "Grouped");

    // A commit whose folder's piece is lost (a name of its own, so that no commit before has its piece): Bass, inside it,
    // is in the commit nowhere that reads, so its line restores nothing, saying why, and Bass stays.
    {
        auto then = phazite::test::copyWholeSong (song);

        for (auto track : phazite::getTrackElements (then))
            if (phazite::readID (track[schema::trackID]) == folder.getRaw())
                track.setProperty (schema::trackName, "Low end, damaged", nullptr);

        const auto damaged =
            commitDamaged (s, then, [&folder] (const juce::ValueTree& element)
                           { return element.hasType (schema::TRACK) && phazite::readID (element[schema::trackID]) == folder.getRaw(); });
        phazite::VersionChange line;
        line.name = "Bass";
        line.track = bass;
        CHECK_FALSE (phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, damaged, line); }));
        CHECK (phazite::findTrackState (song, bass).isValid());
        CHECK (phazite::getRestoreProblem (s.context).contains ("Low end"));
        CHECK (phazite::getRestoreProblem (s.context).contains ("can't be read"));

        // Keys, which the commit holds whole, restores.
        s.setVolume (keys, -9.0f);
        line.name = "Keys";
        line.track = keys;
        CHECK (phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, damaged, line); }));
        CHECK (phazite::getChannelVolume (song, keys) == doctest::Approx (0.0f));
    }

    // A commit whose piece of a plain track is lost besides the folder's (Keys, which can't hold another track): Bass's
    // line is still refused, naming the folder that holds it; a track added since, outside the folder, the commit hasn't,
    // and its line takes it away as added since.
    {
        auto then = phazite::test::copyWholeSong (song);

        for (auto track : phazite::getTrackElements (then))
            if (const auto id = phazite::readID (track[schema::trackID]); id == keys.getRaw() || id == folder.getRaw())
                track.setProperty (schema::trackName, track[schema::trackName].toString() + ", damaged again", nullptr);

        const auto extra = s.addTrack ("Extra");
        const auto damaged = commitDamaged (s, then,
                                            [&] (const juce::ValueTree& element)
                                            {
                                                const auto id = phazite::readID (element[schema::trackID]);
                                                return element.hasType (schema::TRACK) && (id == keys.getRaw() || id == folder.getRaw());
                                            });
        phazite::VersionChange line;
        line.name = "Bass";
        line.track = bass;
        CHECK_FALSE (phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, damaged, line); }));
        CHECK (phazite::getRestoreProblem (s.context).contains ("\"Low end\""));

        // Only Keys lost: Extra's line restores, taking Extra away.
        auto thenAgain = then.createCopy();
        const auto keysOnly =
            commitDamaged (s, thenAgain, [&] (const juce::ValueTree& element)
                           { return element.hasType (schema::TRACK) && phazite::readID (element[schema::trackID]) == keys.getRaw(); });
        line.name = "Extra";
        line.track = extra;
        CHECK (phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, keysOnly, line); }));
        CHECK_FALSE (phazite::findTrackState (song, extra).isValid());
        CHECK_FALSE (phazite::getRestoreProblem (s.context).contains ("Couldn't restore"));
    }

    // A commit whose tempo map stands in for itself (only a piece that isn't Phazite's makes one): the song's own line
    // leaves the tempo map as it is, and no stand-in reaches the song.
    {
        auto then = phazite::test::copyWholeSong (song);
        auto tempoMap = then.getChildWithName (schema::TEMPOMAP);
        REQUIRE (tempoMap.isValid());
        tempoMap.removeAllChildren (nullptr);
        tempoMap.setProperty (schema::versions::missing, true, nullptr);
        const auto damaged = commitDamaged (s, then, [] (const juce::ValueTree&) { return false; });
        const auto mapBefore = song.getState().getChildWithName (schema::TEMPOMAP).createCopy();
        phazite::VersionChange line;
        line.name = "Song";
        line.isSong = true;
        phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, damaged, line); });
        CHECK (song.getState().getChildWithName (schema::TEMPOMAP).isEquivalentTo (mapBefore));
        CHECK_FALSE (song.getState().toXmlString().contains (schema::versions::missing.toString()));
    }
}

namespace
{
// A song with a send from Drums to a return, an EQ on Drums with a curve, and an LFO on Drums that modulates the EQ.
struct Named
{
    TestSong s;
    phazite::TrackID drums = s.addTrack ("Drums");
    phazite::DeviceID eq = s.addEffect (drums, phazite::devices::Equalizer::typeID);
    phazite::TrackID verb;
    phazite::ParameterRef gain { eq, "band4Gain" };

    Named()
    {
        phazite::addSendToNewReturn (s.context, drums);
        verb = phazite::getReturns (s.context).front().id;
        const auto lfo = phazite::addModulator (s.context, drums, phazite::ModulatorType::lfo);
        phazite::modulate (s.context, lfo, gain, 0.5f);
        phazite::addAutomationPoint (s.context, gain, { phazite::BeatPosition::fromBeats (0.0), 0.5f });
        keepVersion (s.context, "Whole");
    }

    phazite::Song& song() { return *phazite::findSong (s.context); }

    bool hasCurve() const
    {
        for (const auto& curve : phazite::findSong (s.context)->getState().getChildWithName (schema::AUTOMATION))
            if (phazite::readID (curve[schema::curveDevice]) == eq.getRaw())
                return true;

        return false;
    }

    bool isModulated() const
    {
        bool found = false;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                found = found || (child.hasType (schema::MODULATES) && phazite::readID (child[schema::modulatesDevice]) == eq.getRaw());
                visit (child);
            }
        };

        visit (phazite::findSong (s.context)->getState());
        return found;
    }

    // A commit of the song as it is whose return's piece and EQ's piece are lost, each given a name of its own first, so
    // that no commit before holds the same piece.
    juce::String commitWithoutReturnAndEq()
    {
        auto then = phazite::test::copyWholeSong (song());
        const std::function<void (juce::ValueTree)> rename = [&] (juce::ValueTree element)
        {
            for (auto child : element)
            {
                if (child.hasType (schema::TRACK) && phazite::readID (child[schema::trackID]) == verb.getRaw())
                    child.setProperty (schema::trackName, "Verb, damaged", nullptr);

                if (child.hasType (schema::DEVICE) && phazite::readID (child[schema::deviceID]) == eq.getRaw())
                    child.setProperty (schema::deviceMix, 0.75, nullptr);

                rename (child);
            }
        };

        rename (then);
        return commitDamaged (s, then,
                              [this] (const juce::ValueTree& element)
                              {
                                  return (element.hasType (schema::TRACK) && phazite::readID (element[schema::trackID]) == verb.getRaw())
                                         || (element.hasType (schema::DEVICE)
                                             && phazite::readID (element[schema::deviceID]) == eq.getRaw());
                              });
    }
};
} // namespace

TEST_CASE (
    "Restoring from a damaged commit keeps what names what its lost pieces held: a send to its return, its device's curve and modulation")
{
    // Found in review (known bug 126's round): the loader's checks, run on a song read in part, took away what named what a
    // lost piece held, though it lived in pieces that read, and the restore then took it out of the song.
    Named n;
    const auto damaged = n.commitWithoutReturnAndEq();
    REQUIRE (phazite::getSends (n.s.context, n.drums).size() == 1);

    SUBCASE ("Restore from This Commit")
    {
        REQUIRE (finished (n.s.context, [&] { return phazite::bringBackVersion (n.s.context, damaged); }));
    }

    SUBCASE ("A line's Restore")
    {
        phazite::VersionChange line;
        line.name = "Drums";
        line.track = n.drums;
        REQUIRE (finished (n.s.context, [&] { return phazite::restoreFromVersion (n.s.context, damaged, line); }));
    }

    CHECK (phazite::getSends (n.s.context, n.drums).size() == 1);
    CHECK (n.hasCurve());
    CHECK (n.isModulated());
    CHECK (phazite::findDeviceState (n.song(), n.eq).isValid());
    checkSong (n.song());
}

TEST_CASE (
    "A commit that holds the curve of a device deleted, which only an Undo needs, restored and undone twice, brings the device and its curve back")
{
    // The loader's checks leave out what names nothing (a curve kept for an Undo that brings its device back), so a commit
    // read back holds less than was committed: nothing is lost by it, as the restore is an undo step of its own.
    Named n;
    phazite::removeDevice (n.s.context, n.eq); // its curve kept for an Undo
    REQUIRE (n.hasCurve());
    const auto without = keepVersion (n.s.context, "Without the EQ");

    // Restored: the curve the commit holds left out of it as it is read, so out of the song too, in the restore's step.
    REQUIRE (finished (n.s.context, [&] { return phazite::bringBackVersion (n.s.context, without); }));
    CHECK_FALSE (n.hasCurve());
    phazite::undo (n.song()); // the restore: the curve back
    CHECK (n.hasCurve());
    phazite::undo (n.song()); // the EQ's removal: the EQ back, with its curve
    CHECK (phazite::findDeviceState (n.song(), n.eq).isValid());
    CHECK (n.hasCurve());
}

TEST_CASE (
    "A restore from a damaged commit leaves the song as its file opens: no curve of a device it hasn't, and Undo brings back what was")
{
    // The second review of known bug 126: a song read in part keeps what names what it couldn't read, so a curve of a device
    // that was in a lost piece, and isn't in the song, came in with the restore and stayed until the project opened again.
    Named n;
    const auto damaged = n.commitWithoutReturnAndEq(); // its EQ's piece lost, its curve read
    phazite::removeDevice (n.s.context, n.eq);
    n.s.reopen (false); // the EQ's curve, kept for an Undo, left out as it opens
    REQUIRE (waitForVersionHistory (n.s.context));
    REQUIRE_FALSE (n.hasCurve());
    const auto before = phazite::test::copyWholeSong (n.song());

    REQUIRE (finished (n.s.context, [&] { return phazite::bringBackVersion (n.s.context, damaged); }));
    CHECK_FALSE (n.hasCurve()); // of a device the song hasn't: taken away again in the restore's step
    checkSong (n.song());

    // Undo brings back exactly what was; Redo the restore again.
    const auto restored = phazite::test::copyWholeSong (n.song());
    phazite::undo (n.song());
    CHECK (phazite::findChanges (before, n.song().getState()).empty());
    phazite::redo (n.song());
    CHECK (phazite::findChanges (restored, n.song().getState()).empty());

    // As its file opens.
    n.s.reopen (false);
    REQUIRE (waitForVersionHistory (n.s.context));
    CHECK (phazite::findChanges (restored, n.song().getState()).empty());
    CHECK_FALSE (n.hasCurve());
}

TEST_CASE ("The other computer's merge of the same commit, merged in after this one's went its own way, brings no item twice")
{
    // The review of S4.5: computer A's merge of idea-2 saved beside computer B's, which gave Pad another ID; B's line merged in
    // afterwards must find Pad as A's, by the IDs each merge's record says it gave, not add it again.
    WaitingMerge m;
    m.merge();
    const auto padHere = m.named ("Pad");
    REQUIRE (padHere.size() == 1);
    REQUIRE (m.s.getProject().save().wasOk());

    // B's merge: the same song, Pad under the ID B gave it, recorded as B's merge records it.
    const auto byB = [&m, &padHere]
    {
        auto song = phazite::test::copyWholeSong (m.song());
        song.removeChild (song.getChildWithName (schema::MERGING), nullptr);
        const auto theirID = padHere[0].getRaw() + 1000;

        for (auto track : phazite::getTrackElements (song))
            if (phazite::readID (track[schema::trackID]) == padHere[0].getRaw())
                track.setProperty (schema::trackID, (juce::int64) theirID, nullptr);

        phazite::VersionStore store (phazite::getVersionsFolder (m.song()), "elsewhere");
        phazite::VersionRecord record;
        record.name = "Merged there";
        record.parents = { m.ours, m.theirs };
        record.renumbered = { { {}, m.pad.getRaw(), theirID } };
        record.branch = findBranch (m.s.context, "main");
        record.saved = juce::Time::currentTimeMillis();
        const auto id = store.writeVersion (record, phazite::splitSong (song));
        REQUIRE (id.isNotEmpty());

        phazite::BranchRecord branch;

        for (const auto& b : phazite::getVersionHistory (m.s.context)->getBranches())
            if (b.id == record.branch)
                branch = b;

        branch.head = id;
        branch.computer = "elsewhere";
        branch.working = {};
        REQUIRE (store.writeBranch (branch));
        store.getPieces().closeFiles();
        return id;
    }();

    m.reopenAs (TestSong::Reopening::withoutSaving);
    REQUIRE (phazite::isMerging (m.s.context));
    keepVersion (m.s.context);
    REQUIRE (phazite::getDivergentVersions (m.s.context) == juce::StringArray (byB));

    // B's line merged in: Pad once, under this song's ID.
    REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::startMerge (m.s.context, byB); }));

    if (! phazite::getMergeLines (m.s.context).empty())
        REQUIRE (phazite::test::finished (m.s.context, [&] { return phazite::finishMerge (m.s.context); }));
    else
        phazite::cancelMerge (m.s.context);

    CHECK (m.named ("Pad") == padHere);
    checkIDs (m.song());
}

namespace
{
// The review of S4.5: idea-2's Pad has the ID c. Line A made Lead with c and merged Pad in under a; line B made Keys with c and
// merged Pad in under b (other), or under a too (its count where A's was). A merging B's line took B's Keys for the parting's
// Pad (this line's record says c is a), and B's Pad, crossed to a, for a second item under a: Pad twice, Keys lost.
void mergeTwoLinesThatMergedPad (bool other)
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto count = song.getState()[schema::nextID];

    const auto named = [&song] (const juce::String& name)
    {
        std::vector<phazite::TrackID> found;

        for (const auto track : phazite::getTrackIDs (song))
            if (phazite::getTrackName (song, track) == name)
                found.push_back (track);

        return found;
    };

    const auto merge = [&s] (const juce::String& version)
    {
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, version); }));
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::finishMerge (s.context); }));
        REQUIRE_MESSAGE (phazite::isMerging (s.context), phazite::getReadingProblem (s.context).toStdString());
        CHECK (phazite::getClashes (s.context).empty());
    };

    // idea-2: Pad, with c.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    const auto pad = s.addTrack ("Pad");
    const auto x = keepVersion (s.context, "Pad");

    // A, on main: Lead with c, then idea-2 merged, Pad given a.
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    song.getState().setProperty (schema::nextID, count, nullptr);
    REQUIRE (s.addTrack ("Lead") == pad);
    keepVersion (s.context, "Lead");
    const auto atMerge = song.getState()[schema::nextID];
    merge (x);
    keepVersion (s.context);
    const auto padA = named ("Pad");
    REQUIRE (padA.size() == 1);
    REQUIRE (padA[0] != pad);

    // B, a line of its own from First: Keys with c, then idea-2 merged, Pad given b (or a, its count where A's was).
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, first, s.save); }));
    REQUIRE (named ("Lead").empty());
    song.getState().setProperty (schema::nextID, count, nullptr);
    REQUIRE (s.addTrack ("Keys") == pad);
    keepVersion (s.context, "Keys");
    song.getState().setProperty (schema::nextID, other ? (juce::int64) atMerge + 1000 : (juce::int64) atMerge, nullptr);
    merge (x);
    const auto byB = keepVersion (s.context);
    REQUIRE (named ("Pad").size() == 1);
    REQUIRE ((named ("Pad")[0] == padA[0]) == ! other);

    // A merges B's line: Pad once, under A's ID; Lead and Keys each once; every ID once.
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    merge (byB);
    CHECK (named ("Pad") == padA);
    CHECK (named ("Lead") == std::vector { pad });
    CHECK (named ("Keys").size() == 1);
    CHECK (named ("Bass") == std::vector { bass });
    checkSong (song);

    // What crossed (B's Pad, to the parting's ID, which A's record takes on to A's) is in the merge waiting, which the song's
    // file holds and its reading again finds; its version records B's Pad by A's ID, or as it is, A's already.
    const auto* waiting = &*phazite::getVersionHistory (s.context)->getMerging();
    const auto crossedTo = [] (const std::vector<phazite::RenumberedID>& ids, phazite::TrackID track)
    { return std::any_of (ids.begin(), ids.end(), [track] (const auto& id) { return id.clip.isEmpty() && id.to == track.getRaw(); }); };
    const auto crossedToA = [&padA, &crossedTo] (const std::vector<phazite::RenumberedID>& ids) { return crossedTo (ids, padA[0]); };
    CHECK (crossedTo (waiting->crossed, pad));
    CHECK (crossedToA (waiting->renumbered));
    CHECK (song.getState().getChildWithName (schema::MERGING).hasProperty (schema::mergingCrossed) == true);

    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));
    auto& reopened = *phazite::findSong (s.context);
    REQUIRE (phazite::isMerging (s.context));
    REQUIRE (dispatchMessagesUntil (
        [&s]
        {
            const auto& found = phazite::getVersionHistory (s.context)->getMerging();
            return found.has_value() && found->readWhole;
        }));
    waiting = &*phazite::getVersionHistory (s.context)->getMerging();
    CHECK (crossedTo (waiting->crossed, pad));
    CHECK (waiting->clashes.empty());

    for (const auto& track : phazite::getTrackElements (waiting->theirs))
        if (track[schema::trackName].toString() == "Pad")
            CHECK (phazite::readID (track[schema::trackID]) == padA[0].getRaw());

    // Its version records the crossing, beside the IDs the merge gave B's Keys.
    keepVersion (s.context);
    const auto* last = phazite::getVersionHistory (s.context)->findLastVersion();
    REQUIRE (last != nullptr);
    CHECK (crossedToA (last->renumbered));
    CHECK (std::any_of (last->renumbered.begin(), last->renumbered.end(),
                        [&pad] (const auto& id) { return id.clip.isEmpty() && id.from == pad.getRaw(); }));
    checkSong (reopened);
}
} // namespace

TEST_CASE ("Two lines that each merged the same version beside an item of their own under the ID it had merge with each item once")
{
    SUBCASE ("each line's merge gave the merged item an ID of its own")
    {
        mergeTwoLinesThatMergedPad (true);
    }

    SUBCASE ("both lines' merges gave it the same ID")
    {
        mergeTwoLinesThatMergedPad (false);
    }
}

namespace
{
// Known bug 199: lines that count their IDs on from the same place (two computers, or a line and the line it merged into)
// and merge each other's work, in a project of their own.
struct Lines
{
    TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);

    juce::String branch() const { return phazite::getCurrentBranch (s.context)->id; }
    juce::var count() const { return song.getState()[schema::nextID]; }
    void countFrom (const juce::var& next) { song.getState().setProperty (schema::nextID, next, nullptr); }

    // A new branch from the song as it is, or from a version; and the branch this one is, the song made its last version's.
    juce::String newLine (const juce::String& from = {})
    {
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, from, s.save); }));
        return branch();
    }

    void toBranch (const juce::String& id)
    {
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, id, s.save); }));
    }

    // A version merged into the song, every line of it, without a clash, waiting for its commit.
    void merge (const juce::String& version)
    {
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, version); }));
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::finishMerge (s.context); }));
        REQUIRE_MESSAGE (phazite::isMerging (s.context), phazite::getReadingProblem (s.context).toStdString());
        CHECK (phazite::getClashes (s.context).empty());
    }

    std::vector<phazite::TrackID> named (const juce::String& name) const
    {
        std::vector<phazite::TrackID> found;

        for (const auto track : phazite::getTrackIDs (song))
            if (phazite::getTrackName (song, track) == name)
                found.push_back (track);

        return found;
    }

    juce::String typeOf (phazite::DeviceID device) const { return phazite::findDeviceState (song, device)[schema::deviceType].toString(); }

    // The curves that name a device.
    int countCurves (phazite::DeviceID device) const
    {
        int curves = 0;

        for (const auto& curve : song.getState().getChildWithName (schema::AUTOMATION))
            curves += phazite::readID (curve[schema::curveDevice]) == device.getRaw() ? 1 : 0;

        return curves;
    }

    // Every clip of the song, and a clip's notes by their pitches, each pitch's IDs.
    std::vector<juce::ValueTree> clips() const
    {
        std::vector<juce::ValueTree> found;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::CLIP))
                    found.push_back (child);
                else
                    visit (child);
            }
        };

        visit (song.getState());
        return found;
    }

    juce::ValueTree findClip (std::uint64_t id) const
    {
        for (const auto& clip : clips())
            if (phazite::readID (clip[schema::clipID]) == id)
                return clip;

        return {};
    }

    static std::map<int, std::vector<std::uint64_t>> notesOf (const juce::ValueTree& clip)
    {
        std::map<int, std::vector<std::uint64_t>> notes;

        for (const auto& note : clip)
            if (note.hasType (schema::NOTE))
                notes[(int) note[schema::notePitch]].push_back (phazite::readID (note[schema::noteID]));

        return notes;
    }
};
} // namespace

TEST_CASE ("A line merged back into the line that merged it, whose merge gave its item new IDs, merges each item once (known bug 199)")
{
    // main merged idea-2, whose Pad and its EQ had the IDs of main's Lead and its synth, so the merge gave them new ones. idea-2
    // goes on, Pad renamed, and merges main: Pad stays idea-2's, renamed, with its EQ; main's Lead comes in once, under IDs of
    // its own, with its synth and the sidechain that names it. Cancel Merge and Undo bring the song back exactly.
    Lines l;
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    const auto compressor = l.s.addEffect (bass, phazite::devices::Compressor::typeID);
    keepVersion (l.s.context, "First");
    const auto count = l.count();

    const auto idea = l.newLine();
    const auto pad = l.s.addTrack ("Pad");
    const auto eq = l.s.addEffect (pad, phazite::devices::Equalizer::typeID);
    const auto x = keepVersion (l.s.context, "Pad");

    l.toBranch (mainLine);
    l.countFrom (count);
    const auto lead = l.s.addTrack ("Lead");
    REQUIRE (lead == pad);
    REQUIRE (l.s.setInstrument (lead, phazite::devices::Synth::typeID).getRaw() == eq.getRaw());
    phazite::setSidechainSource (l.s.context, compressor, lead);
    REQUIRE (phazite::getSidechainSource (l.s.context, compressor) == lead);
    keepVersion (l.s.context, "Lead");
    l.merge (x);
    const auto merged = keepVersion (l.s.context);
    REQUIRE (l.named ("Pad").size() == 1);
    REQUIRE (l.named ("Pad")[0] != pad);
    REQUIRE (phazite::getDevices (l.song, l.named ("Pad")[0]) != std::vector { eq });

    l.toBranch (idea);
    l.s.rename (pad, "Pad 2");
    keepVersion (l.s.context, "Pad 2");
    const auto before = phazite::test::copyWholeSong (l.song);

    REQUIRE (phazite::test::finished (l.s.context, [&] { return phazite::startMerge (l.s.context, merged); }));
    UndoCheck undo (l.s.context);
    undo.check ("Merge", [&l] { REQUIRE (phazite::test::finished (l.s.context, [&l] { return phazite::finishMerge (l.s.context); })); });
    REQUIRE (phazite::isMerging (l.s.context));
    CHECK (phazite::getClashes (l.s.context).empty());

    CHECK (l.named ("Pad 2") == std::vector { pad });
    CHECK (l.named ("Pad").empty());
    CHECK (phazite::getDevices (l.song, pad) == std::vector { eq });
    const auto leads = l.named ("Lead");
    REQUIRE (leads.size() == 1);
    CHECK (leads[0] != pad);
    const auto leadDevices = phazite::getDevices (l.song, leads[0]);
    REQUIRE (leadDevices.size() == 1);
    CHECK (leadDevices[0] != eq);
    CHECK (l.typeOf (leadDevices[0]) == phazite::devices::Synth::typeID);
    CHECK (phazite::getSidechainSource (l.s.context, compressor) == leads[0]);
    CHECK (l.named ("Bass") == std::vector { bass });
    checkSong (l.song);

    // Cancel Merge: the song as it was before.
    REQUIRE (phazite::canCancelMerge (l.s.context));
    phazite::cancelMerge (l.s.context);
    CHECK_FALSE (phazite::isMerging (l.s.context));
    CHECK (phazite::findChanges (before, l.song.getState()).empty());
    CHECK (l.named ("Pad 2") == std::vector { pad });
    CHECK (l.named ("Lead").empty());
}

TEST_CASE ("Two lines that each merged the same version, where only one line's merge gave a device of it a new ID, merge with "
           "each device once (known bug 199)")
{
    // idea-2's Pad has an EQ, whose gain has a curve. main's Lead had Pad's IDs but not its EQ's, so main's merge gave Pad new
    // IDs and kept the EQ's; another line's Keys had Pad's IDs and an EQ with its EQ's, so that line's merge gave both new
    // ones. main merges that line: Pad once, with its EQ under main's ID and the curve on it; Keys once, with its own EQ.
    Lines l;
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    const auto first = keepVersion (l.s.context, "First");
    const auto count = l.count();

    l.newLine();
    const auto pad = l.s.addTrack ("Pad");
    const auto eq = l.s.addEffect (pad, phazite::devices::Equalizer::typeID);
    REQUIRE (phazite::addAutomationPoint (l.s.context, { eq, "band4Gain" }, { phazite::BeatPosition::fromBeats (0.0), 0.5f }) >= 0);
    const auto x = keepVersion (l.s.context, "Pad");

    l.toBranch (mainLine);
    l.countFrom (count);
    REQUIRE (l.s.addTrack ("Lead") == pad);
    keepVersion (l.s.context, "Lead");
    const auto atMerge = l.count();
    l.merge (x);
    keepVersion (l.s.context);
    const auto padHere = l.named ("Pad");
    REQUIRE (padHere.size() == 1);
    REQUIRE (padHere[0] != pad);
    REQUIRE (phazite::getDevices (l.song, padHere[0]) == std::vector { eq });

    l.newLine (first);
    l.countFrom (count);
    const auto keys = l.s.addTrack ("Keys");
    REQUIRE (keys == pad);
    REQUIRE (l.s.addEffect (keys, phazite::devices::Equalizer::typeID) == eq);
    keepVersion (l.s.context, "Keys");
    l.countFrom ((juce::int64) atMerge + 1000);
    l.merge (x);
    const auto other = keepVersion (l.s.context);
    REQUIRE (l.named ("Pad").size() == 1);
    REQUIRE (phazite::getDevices (l.song, l.named ("Pad")[0]).size() == 1);
    REQUIRE (phazite::getDevices (l.song, l.named ("Pad")[0])[0] != eq);

    l.toBranch (mainLine);
    l.merge (other);
    CHECK (l.named ("Pad") == padHere);
    CHECK (phazite::getDevices (l.song, padHere[0]) == std::vector { eq });
    CHECK (l.countCurves (eq) == 1);
    const auto keysHere = l.named ("Keys");
    REQUIRE (keysHere.size() == 1);
    const auto keysDevices = phazite::getDevices (l.song, keysHere[0]);
    REQUIRE (keysDevices.size() == 1);
    CHECK (keysDevices[0] != eq);
    CHECK (l.countCurves (keysDevices[0]) == 0);
    CHECK (l.named ("Lead") == std::vector { pad });
    CHECK (phazite::getDevices (l.song, pad).empty());
    CHECK (l.named ("Bass") == std::vector { bass });
    checkSong (l.song);
}

TEST_CASE ("A line whose merge of a version kept an item's IDs merges a line whose merge of it gave that item new ones with each "
           "item once (known bug 199)")
{
    // idea-2's Pad had the IDs of main's Lead, which the compressor on Bass is sidechained to, so main's merge gave Pad new
    // ones; another line merged idea-2 with Pad under its own IDs, then merges main: Pad once, as that line has it, with its
    // EQ; main's Lead once, under IDs of its own, which the sidechain names.
    Lines l;
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    const auto compressor = l.s.addEffect (bass, phazite::devices::Compressor::typeID);
    const auto first = keepVersion (l.s.context, "First");
    const auto count = l.count();

    l.newLine();
    const auto pad = l.s.addTrack ("Pad");
    const auto eq = l.s.addEffect (pad, phazite::devices::Equalizer::typeID);
    const auto x = keepVersion (l.s.context, "Pad");

    l.toBranch (mainLine);
    l.countFrom (count);
    const auto lead = l.s.addTrack ("Lead");
    REQUIRE (lead == pad);
    phazite::setSidechainSource (l.s.context, compressor, lead);
    REQUIRE (phazite::getSidechainSource (l.s.context, compressor) == lead);
    keepVersion (l.s.context, "Lead");
    l.merge (x);
    const auto fromMain = keepVersion (l.s.context);
    REQUIRE (l.named ("Pad").size() == 1);
    REQUIRE (l.named ("Pad")[0] != pad);

    l.newLine (first);
    l.s.setVolume (bass, -3.0f);
    keepVersion (l.s.context, "Quieter");
    l.merge (x);
    keepVersion (l.s.context);
    REQUIRE (l.named ("Pad") == std::vector { pad });

    l.merge (fromMain);
    CHECK (l.named ("Pad") == std::vector { pad });
    CHECK (phazite::getDevices (l.song, pad) == std::vector { eq });
    const auto leads = l.named ("Lead");
    REQUIRE (leads.size() == 1);
    CHECK (leads[0] != pad);
    CHECK (phazite::getDevices (l.song, leads[0]).empty());
    CHECK (phazite::getSidechainSource (l.s.context, compressor) == leads[0]);
    CHECK (l.named ("Bass") == std::vector { bass });
    checkSong (l.song);
}

TEST_CASE ("A note a merge gave a new ID, in a clip an earlier merge gave a new ID, is the line's own note again when that "
           "line merges back (known bug 199)")
{
    // idea-2 made a clip on Bass with the ID of main's own clip there, so main's merge gave it a new one. Each line then put
    // a note in it with the same ID, so main's next merge gave idea-2's note a new one, recorded in that clip as main numbers
    // it. idea-2 merges main: its clip and its note keep their IDs; main's own clip and note come in under IDs of their own.
    Lines l;
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    keepVersion (l.s.context, "First");
    const auto count = l.count();

    const auto idea = l.newLine();
    const auto riff = l.s.addMidiClip (bass, 0.0, 4.0);
    l.s.addNote (riff, 60, 0.0, 1.0);
    const auto x1 = keepVersion (l.s.context, "Riff");

    l.toBranch (mainLine);
    l.countFrom (count);
    const auto own = l.s.addMidiClip (bass, 8.0, 4.0);
    REQUIRE (own == riff);
    l.s.addNote (own, 48, 8.0, 1.0);
    keepVersion (l.s.context, "A clip of main's");
    l.merge (x1);
    keepVersion (l.s.context);
    auto riffHere = phazite::ClipID();

    for (const auto& clip : l.clips())
        if (Lines::notesOf (clip).count (60) > 0)
            riffHere = phazite::ClipID::fromRaw (phazite::readID (clip[schema::clipID]));

    REQUIRE (riffHere.isValid());
    REQUIRE (riffHere != riff);

    // A note in it on each line, with the same ID.
    l.toBranch (idea);
    const auto noteCount = l.count();
    l.s.addNote (riff, 64, 1.0, 1.0);
    const auto x2 = keepVersion (l.s.context, "A note more");
    const auto ideaNotes = Lines::notesOf (l.findClip (riff.getRaw()));
    REQUIRE (ideaNotes.count (64) == 1);
    const auto note = ideaNotes.at (64).front();

    l.toBranch (mainLine);
    const auto mainCount = l.count();
    l.countFrom (noteCount);
    l.s.addNote (riffHere, 67, 2.0, 1.0);
    l.countFrom (mainCount);
    REQUIRE (Lines::notesOf (l.findClip (riffHere.getRaw())).at (67) == std::vector { note });
    keepVersion (l.s.context, "A note of main's");
    l.merge (x2);
    const auto fromMain = keepVersion (l.s.context);
    const auto mainNotes = Lines::notesOf (l.findClip (riffHere.getRaw()));
    REQUIRE (mainNotes.count (64) == 1);
    REQUIRE (mainNotes.at (64).front() != note);

    l.toBranch (idea);
    l.s.setVolume (bass, -3.0f);
    keepVersion (l.s.context, "Quieter");
    l.merge (fromMain);

    REQUIRE (l.clips().size() == 2);
    const auto notes = Lines::notesOf (l.findClip (riff.getRaw()));
    CHECK (notes.size() == 3);
    CHECK (notes.count (60) == 1);
    REQUIRE (notes.count (64) == 1);
    CHECK (notes.at (64) == std::vector { note });
    REQUIRE (notes.count (67) == 1);
    CHECK (notes.at (67).size() == 1);
    CHECK (notes.at (67) != std::vector { note });

    for (const auto& clip : l.clips())
    {
        if (phazite::readID (clip[schema::clipID]) != riff.getRaw())
        {
            const auto others = Lines::notesOf (clip);
            CHECK (others.size() == 1);
            CHECK (others.count (48) == 1);
        }
    }

    checkSong (l.song);
}

namespace
{
// idea-2 made Pad with the IDs main's Lead and C's Keys have, then Rhodes with an EQ; main and C each merged both versions,
// which gave Pad and Rhodes new IDs on each line (C, with Bass 2 too, gave Pad others, and took Bass 2 out before it merged
// Rhodes, which kept its ID there: the parting's Rhodes, under an ID main's records move on). Each track has a volume of its
// own.
struct ThreeLines
{
    Lines l;
    std::size_t tracksAtStart = phazite::getTrackIDs (l.song).size();
    juce::String mainLine = l.branch(), idea, c, first, v, v2, fromC;
    phazite::TrackID bass, pad, lead, keys;
    std::vector<phazite::TrackID> padHere, rhodesHere;

    explicit ThreeLines (bool cTakesOutBass2)
    {
        bass = l.s.addTrack ("Bass");
        first = keepVersion (l.s.context, "First");
        const auto count = l.count();

        idea = l.newLine();
        pad = l.s.addTrack ("Pad");
        l.s.setVolume (pad, -1.0f);
        v = keepVersion (l.s.context, "Pad");

        l.toBranch (mainLine);
        l.countFrom (count);
        lead = l.s.addTrack ("Lead");
        REQUIRE (lead == pad);
        l.s.setVolume (lead, -4.0f);
        keepVersion (l.s.context, "Lead");
        const auto atMerge = l.count();
        l.merge (v);
        keepVersion (l.s.context);
        const auto padOnMain = l.named ("Pad");
        REQUIRE (padOnMain.size() == 1);

        // Rhodes with the IDs main's merge gave Pad, as idea-2 counting on from where main's count was would give it.
        l.toBranch (idea);
        l.countFrom ((juce::int64) padOnMain[0].getRaw());
        const auto rhodesTrack = l.s.addTrack ("Rhodes");
        REQUIRE (rhodesTrack == padOnMain[0]);
        l.s.setVolume (rhodesTrack, -2.0f);
        l.s.addEffect (rhodesTrack, phazite::devices::Equalizer::typeID);
        v2 = keepVersion (l.s.context, "Rhodes");
        const auto rhodes = l.named ("Rhodes");

        l.toBranch (mainLine);
        l.merge (v2);
        keepVersion (l.s.context);
        padHere = l.named ("Pad");
        rhodesHere = l.named ("Rhodes");
        REQUIRE (padHere.size() == 1);
        REQUIRE (rhodesHere.size() == 1);
        REQUIRE (padHere[0] != pad);
        REQUIRE (rhodesHere != rhodes);

        // C counts on from where main did, so that its merge of Pad gives it main's IDs; with Bass 2, others.
        c = l.newLine (first);
        l.countFrom (count);
        keys = l.s.addTrack ("Keys");
        REQUIRE (keys == pad);
        l.s.setVolume (keys, -3.0f);
        const auto bass2 = cTakesOutBass2 ? l.s.addTrack ("Bass 2") : phazite::TrackID();
        keepVersion (l.s.context, "Keys");

        if (! cTakesOutBass2)
            l.countFrom (atMerge);

        l.merge (v);
        keepVersion (l.s.context);

        if (cTakesOutBass2)
        {
            phazite::deleteTracks (l.song, { bass2 }, nullptr);
            keepVersion (l.s.context, "No Bass 2");
        }

        l.merge (v2);
        fromC = keepVersion (l.s.context);
        REQUIRE (l.named ("Pad").size() == 1);
        REQUIRE ((l.named ("Rhodes") == rhodes) == cTakesOutBass2);
        REQUIRE ((l.named ("Pad") == padHere) == ! cTakesOutBass2);
        l.toBranch (mainLine);
    }

    float volume (phazite::TrackID track) const { return phazite::getChannelVolume (l.song, track); }

    // main as it is after merging C: every track once, with its own volume and devices, Keys under IDs of its own.
    phazite::TrackID checkMerged (const juce::String& keysName = "Keys")
    {
        CHECK (l.named ("Pad") == padHere);
        CHECK (l.named ("Rhodes") == rhodesHere);
        CHECK (l.named ("Lead") == std::vector { lead });
        CHECK (l.named ("Bass") == std::vector { bass });
        CHECK (l.named ("Bass 2").empty());
        const auto keysHere = l.named (keysName);
        REQUIRE (keysHere.size() == 1);
        CHECK (keysHere[0] != lead);
        CHECK (phazite::getTrackIDs (l.song).size() == tracksAtStart + 5);
        CHECK (volume (rhodesHere[0]) == doctest::Approx (-2.0f));
        CHECK (volume (lead) == doctest::Approx (-4.0f));
        CHECK (volume (keysHere[0]) == doctest::Approx (-3.0f));
        const auto rhodesDevices = phazite::getDevices (l.song, rhodesHere[0]);
        REQUIRE (rhodesDevices.size() == 1);
        CHECK (l.typeOf (rhodesDevices[0]) == phazite::devices::Equalizer::typeID);
        CHECK (phazite::getDevices (l.song, padHere[0]).empty());
        CHECK (phazite::getDevices (l.song, keysHere[0]).empty());
        CHECK (phazite::getDevices (l.song, lead).empty());
        checkSong (l.song);
        return keysHere[0];
    }
};
} // namespace

TEST_CASE ("A line that merged two versions of another merges a line that merged the same two beside an item of its own with each "
           "item once, and again after that line went on (known bug 199)")
{
    // main merges C: C's Pad and Rhodes cross to the parting's IDs, which main's records take on to main's, each once, not
    // twice; Keys comes in under IDs of its own. C goes on, and main merges it again by what that merge recorded.
    ThreeLines t (false);
    t.l.merge (t.fromC);
    CHECK (t.volume (t.padHere[0]) == doctest::Approx (-1.0f));
    const auto keysHere = t.checkMerged();
    keepVersion (t.l.s.context);

    t.l.toBranch (t.c);
    t.l.s.rename (t.keys, "Keys 2");
    t.l.s.setVolume (t.l.named ("Pad")[0], -6.0f);
    const auto again = keepVersion (t.l.s.context, "Keys 2");

    t.l.toBranch (t.mainLine);
    t.l.merge (again);
    CHECK (t.checkMerged ("Keys 2") == keysHere);
    CHECK (t.l.named ("Keys").empty());
    CHECK (t.volume (t.padHere[0]) == doctest::Approx (-6.0f));
}

TEST_CASE ("A line that merged two versions of another merges a line whose own merge alone moved one of their items, which holds "
           "the parting's other item under the ID this line's merge gave the first, with each item once (known bug 199)")
{
    // C holds the parting's Rhodes under the ID main gave Pad: it is the parting's Rhodes, which main's records move on, not an
    // item of C's own in the way of C's Pad.
    ThreeLines t (true);
    t.l.merge (t.fromC);
    CHECK (t.volume (t.padHere[0]) == doctest::Approx (-1.0f));
    t.checkMerged();
}

TEST_CASE ("A line merged back, then merged again both ways after both lines went on, holds each item and note once with its own "
           "content (known bug 199)")
{
    // main merged idea-2, giving its Pad and its note in Bass's riff new IDs (main's Lead and note had them); idea-2 merged
    // main back, then both went on, each renaming and adding a note to the riff; main merges idea-2 again, then idea-2 main.
    Lines l;
    const auto tracksAtStart = phazite::getTrackIDs (l.song).size();
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    const auto riff = l.s.addMidiClip (bass, 0.0, 4.0);
    l.s.addNote (riff, 60, 0.0, 1.0);
    keepVersion (l.s.context, "First");
    const auto count = l.count();

    const auto idea = l.newLine();
    const auto pad = l.s.addTrack ("Pad");
    l.s.addNote (riff, 64, 1.0, 1.0);
    const auto x = keepVersion (l.s.context, "Pad");
    const auto note = Lines::notesOf (l.findClip (riff.getRaw())).at (64).front();

    l.toBranch (mainLine);
    l.countFrom (count);
    const auto lead = l.s.addTrack ("Lead");
    REQUIRE (lead == pad);
    l.s.addNote (riff, 67, 2.0, 1.0);
    REQUIRE (Lines::notesOf (l.findClip (riff.getRaw())).at (67) == std::vector { note });
    keepVersion (l.s.context, "Lead");
    l.merge (x);
    const auto merged = keepVersion (l.s.context);
    const auto padHere = l.named ("Pad");
    REQUIRE (padHere.size() == 1);
    REQUIRE (padHere[0] != pad);

    // Each pitch once in the riff, the owned pitch under the ID both lines gave their own notes, the other's under another;
    // Pad and Lead once.
    const auto checkLine = [&l, &riff, &note, tracksAtStart] (phazite::TrackID padID, const juce::String& padName,
                                                              const juce::String& leadName, int ownedPitch, int otherPitch,
                                                              const std::vector<int>& pitches)
    {
        CHECK (l.named (padName) == std::vector { padID });
        REQUIRE (l.named (leadName).size() == 1);
        CHECK (l.named (leadName)[0] != padID);
        CHECK (l.named ("Bass").size() == 1);
        CHECK (phazite::getTrackIDs (l.song).size() == tracksAtStart + 3);
        REQUIRE (l.clips().size() == 1);
        const auto notes = Lines::notesOf (l.findClip (riff.getRaw()));
        std::vector<int> held;

        for (const auto& [pitch, ids] : notes)
        {
            held.push_back (pitch);
            CHECK (ids.size() == 1);
        }

        CHECK (held == pitches);
        REQUIRE (notes.count (ownedPitch) == 1);
        REQUIRE (notes.count (otherPitch) == 1);
        CHECK (notes.at (ownedPitch) == std::vector { note });
        CHECK (notes.at (otherPitch) != std::vector { note });
        checkSong (l.song);
    };

    l.toBranch (idea);
    l.merge (merged);
    checkLine (pad, "Pad", "Lead", 64, 67, { 60, 64, 67 });
    const auto leadThere = l.named ("Lead")[0];
    keepVersion (l.s.context);

    l.s.rename (pad, "Pad 2");
    l.s.addNote (riff, 72, 3.0, 1.0);
    const auto ideaOn = keepVersion (l.s.context, "Pad 2");

    l.toBranch (mainLine);
    l.s.rename (lead, "Lead 2");
    l.s.addNote (riff, 74, 3.5, 0.5);
    keepVersion (l.s.context, "Lead 2");
    l.merge (ideaOn);
    checkLine (padHere[0], "Pad 2", "Lead 2", 67, 64, { 60, 64, 67, 72, 74 });
    CHECK (l.named ("Lead 2") == std::vector { lead });
    const auto mainOn = keepVersion (l.s.context);

    l.toBranch (idea);
    l.merge (mainOn);
    checkLine (pad, "Pad 2", "Lead 2", 64, 67, { 60, 64, 67, 72, 74 });
    CHECK (l.named ("Lead 2") == std::vector { leadThere });
}

TEST_CASE ("A line merged back, whose merge gave its item a new ID and took that item out since, keeps its own item under the ID "
           "it gave away apart (known bug 199)")
{
    // main merged idea-2, giving its Pad new IDs (main's Lead had them), then took Pad out. idea-2 merges main: main's Lead
    // isn't the parting's Pad, which main took out: Pad goes, Lead comes in once under IDs of its own.
    Lines l;
    const auto tracksAtStart = phazite::getTrackIDs (l.song).size();
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    keepVersion (l.s.context, "First");
    const auto count = l.count();

    const auto idea = l.newLine();
    const auto pad = l.s.addTrack ("Pad");
    const auto eq = l.s.addEffect (pad, phazite::devices::Equalizer::typeID);
    const auto x = keepVersion (l.s.context, "Pad");

    l.toBranch (mainLine);
    l.countFrom (count);
    const auto lead = l.s.addTrack ("Lead");
    REQUIRE (lead == pad);
    l.s.setVolume (lead, -4.0f);
    keepVersion (l.s.context, "Lead");
    l.merge (x);
    keepVersion (l.s.context);
    REQUIRE (l.named ("Pad").size() == 1);
    phazite::deleteTracks (l.song, l.named ("Pad"), nullptr);
    const auto noPad = keepVersion (l.s.context, "No Pad");

    l.toBranch (idea);
    l.s.setVolume (bass, -3.0f);
    keepVersion (l.s.context, "Quieter");
    l.merge (noPad);

    CHECK (l.named ("Pad").empty());
    CHECK_FALSE (phazite::findDeviceState (l.song, eq).isValid());
    const auto leads = l.named ("Lead");
    REQUIRE (leads.size() == 1);
    CHECK (leads[0] != pad);
    CHECK (phazite::getChannelVolume (l.song, leads[0]) == doctest::Approx (-4.0f));
    CHECK (phazite::getDevices (l.song, leads[0]).empty());
    CHECK (l.named ("Bass") == std::vector { bass });
    CHECK (phazite::getChannelVolume (l.song, bass) == doctest::Approx (-3.0f));
    CHECK (phazite::getTrackIDs (l.song).size() == tracksAtStart + 2);
    checkSong (l.song);
}

TEST_CASE ("A merge whose songs, matched by the IDs earlier merges gave anew, would hold two items under one ID is refused, and "
           "nothing changes (known bug 199)")
{
    // Records no merge of these lines wrote (another tool's): idea-2's merge says it gave its Pad an ID neither song holds, and
    // main's merge says it gave that ID Bass's. Crossed so, Pad would meet Bass, which both lines hold as the parting has it,
    // and the merge would make them one: it says why, and merges nothing.
    Lines l;
    const auto mainLine = l.branch();
    const auto bass = l.s.addTrack ("Bass");
    const auto first = keepVersion (l.s.context, "First");
    constexpr std::uint64_t far = 1000000; // no ID either song holds

    const auto idea = l.newLine();
    const auto pad = l.s.addTrack ("Pad");
    const auto x = keepVersion (l.s.context, "Pad");
    const auto theirMerge = commitElsewhere (l.s, idea, { x, first }, { { {}, far, pad.getRaw() } });

    l.toBranch (mainLine);
    l.s.addTrack ("Lead");
    const auto withLead = keepVersion (l.s.context, "Lead");
    const auto ourMerge = commitElsewhere (l.s, mainLine, { withLead, first }, { { {}, far, bass.getRaw() } });
    REQUIRE (l.s.getProject().save().wasOk());
    REQUIRE (waitForVersionHistory (l.s.context));
    REQUIRE (l.s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (l.s.context));
    // The song, which is that version's, grows from it as it opens.
    REQUIRE (phazite::getVersionHistory (l.s.context)->findLastVersion()->id == ourMerge);
    REQUIRE (phazite::getVersionHistory (l.s.context)->getBaseVersionID() == ourMerge);

    auto& song = *phazite::findSong (l.s.context);
    const auto before = phazite::test::copyWholeSong (song);
    CHECK_FALSE (phazite::test::finished (l.s.context, [&l, &theirMerge] { return phazite::startMerge (l.s.context, theirMerge); }));
    CHECK_FALSE (phazite::isMerging (l.s.context));
    CHECK (phazite::getMergeLines (l.s.context).empty());
    CHECK (phazite::getReadingProblem (l.s.context).contains ("Nothing was merged"));
    CHECK (phazite::getReadingProblem (l.s.context).contains ("one ID for two"));
    CHECK (phazite::findChanges (before, song.getState()).empty());
    CHECK (song.getState()[schema::nextID] == before[schema::nextID]);
    checkSong (song);
}

TEST_CASE ("A note's conflict in an alias group is listed once, and each choice of it reaches every clip of the group")
{
    // X on the Bass and its alias Y; idea-2 transposes n by 5 through Y, main by 2 through X: one conflict, the group's keeper's
    // (SongMerge.h), whose choice every clip of the group takes, as Undo puts it back in every one.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto clip = s.addMidiClip (bass, 0.0, 8.0);
    s.addNote (clip, 36, 0.0, 1.0);
    const auto alias = s.copyClip (clip, 8.0, true);
    keepVersion (s.context, "First");
    const auto note = phazite::getClipNotes (*phazite::findSong (s.context), clip).front().id;

    const auto transpose = [&s, note] (phazite::ClipID through, int semitones)
    {
        auto& song = *phazite::findSong (s.context);
        const phazite::UndoStep step (song, "Transpose");
        phazite::transposeNotes (song, { { through, note } }, semitones);
    };

    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    transpose (alias, 5);
    const auto theirs = keepVersion (s.context, "A fourth up");
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    transpose (clip, 2);
    keepVersion (s.context, "A tone up");

    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
    phazite::finishMerge (s.context);
    REQUIRE (phazite::isMerging (s.context));
    REQUIRE (phazite::getClashes (s.context).size() == 1);

    const auto pitchIn = [&s, note] (phazite::ClipID in)
    { return phazite::getNoteData (*phazite::findSong (s.context), phazite::NoteRef { in, note }).value().pitch; };

    const auto checkHoldsTogether = [&] (int pitch)
    {
        auto& song = *phazite::findSong (s.context);
        CHECK (pitchIn (clip) == pitch);
        CHECK (pitchIn (alias) == pitch);
        CHECK (describeClipNotes (song, clip) == describeClipNotes (song, alias));
        checkSong (song);
    };

    checkHoldsTogether (38);

    for (const auto [value, pitch] : { std::pair { 2, 41 }, std::pair { 0, 36 }, std::pair { 1, 38 }, std::pair { 2, 41 } })
    {
        INFO ("chosen " << value);
        REQUIRE (phazite::chooseClash (s.context, 0, value));
        CHECK (phazite::getClashes (s.context)[0].chosen == value);
        checkHoldsTogether (pitch);
    }

    phazite::undo (*phazite::findSong (s.context)); // back to this branch's
    checkHoldsTogether (38);
}
