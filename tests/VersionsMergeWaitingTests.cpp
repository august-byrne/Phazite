// SPDX-License-Identifier: AGPL-3.0-or-later
// A merge taken into the song that waits for its commit (docs/design/versions.md: Merging; git's MERGE_HEAD): kept in the song
// (Schema.h: MERGING), so that whichever file of the song opens (the project file saved or not, its autosave after a crash)
// holds the merge state its song was in; waiting on top of a branch that moved on, and no more where the branch holds the
// merged commit already; never two at once; and restores from a damaged commit that can't tell where a line was.
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/PieceStore.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

#include <algorithm>

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
juce::String commitElsewhere (WaitingMerge& m, const juce::String& branchID, juce::StringArray parents,
                              std::vector<phazite::RenumberedID> renumbered = {})
{
    const auto folder = phazite::getVersionsFolder (m.song());
    phazite::VersionStore store (folder, "elsewhere");
    phazite::VersionRecord record;
    record.name = "From elsewhere";
    record.parents = parents;
    record.renumbered = std::move (renumbered);
    record.branch = branchID;
    record.saved = juce::Time::currentTimeMillis();
    const auto song = phazite::getVersionHistory (m.s.context)->readSong (parents[0]);
    REQUIRE (song.isValid());
    const auto id = store.writeVersion (record, phazite::splitSong (song));
    REQUIRE (id.isNotEmpty());

    phazite::BranchRecord branch;

    for (const auto& b : phazite::getVersionHistory (m.s.context)->getBranches())
        if (b.id == branchID)
            branch = b;

    branch.head = id;
    branch.computer = "elsewhere";
    branch.working = {};
    REQUIRE (store.writeBranch (branch));
    store.getPieces().closeFiles();
    return id;
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
        CHECK_FALSE (phazite::hasSomethingToCommit (m.s.context));

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
    const auto lines = phazite::getChangesSinceLastVersion (m.s.context);
    CHECK_FALSE (lines.empty());
    CHECK (std::none_of (lines.begin(), lines.end(), [] (const phazite::VersionChange& line) { return line.isSong; }));

    // Each line discarded, the song holds what it was saved with but the merge waiting: that alone is an unsaved change, and
    // something to save a version of.
    for (const auto& line : lines)
        REQUIRE (phazite::takeBack (m.s.context, line));

    REQUIRE (dispatchMessagesUntil ([&m] { return ! phazite::hasChangesSinceLastVersion (m.s.context); }));
    REQUIRE (phazite::isMerging (m.s.context));
    CHECK (m.s.getProject().hasUnsavedChanges());
    CHECK (phazite::hasSomethingToCommit (m.s.context));
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

TEST_CASE ("A merge waiting while another computer commits on the branch waits on top of that commit; one it merged there waits no more")
{
    // Another computer's commit on main, on top of the one the merge was taken at: the merge waits on top of it, and says so.
    {
        WaitingMerge m;
        m.merge();
        REQUIRE (m.s.getProject().save().wasOk());
        const auto elsewhere = commitElsewhere (m, findBranch (m.s.context, "main"), { m.ours });
        m.reopenAs (TestSong::Reopening::withoutSaving);
        REQUIRE (m.lastCommit().id == elsewhere);
        REQUIRE (phazite::isMerging (m.s.context));
        CHECK (phazite::getMergeProblem (m.s.context).contains ("moved on"));
        keepVersion (m.s.context);
        CHECK (m.lastCommit().parents == juce::StringArray (elsewhere, m.theirs));
        CHECK (sorted (m.lastCommit().renumbered) == sorted (m.given));
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
    // Computer A (this one) merged idea-2's Pad as one ID; computer B merged it too, as another, and committed. A commits on
    // top of B's merge, recording its own IDs; then idea-2 goes on, Pad renamed, or deleted, and A merges it again: Pad
    // once, renamed, or gone.
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
        CHECK (m.lastCommit().parents == juce::StringArray (byB, m.theirs));
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

namespace
{
// The pack the store would write a song's pieces it hasn't yet into, made a folder (with a file in it, so that it can't
// be replaced either): the next write of those pieces fails, as a full or locked drive makes it.
juce::File blockPackFor (const juce::ValueTree& song, const juce::File& versionsFolder, const juce::File& scratch)
{
    const phazite::PieceStore store (versionsFolder.getChildFile ("pieces"));
    std::map<juce::String, juce::MemoryBlock> fresh;

    for (const auto& [name, piece] : phazite::splitSong (song).pieces)
        if (! store.read (name).has_value())
            fresh.emplace (name, piece);

    store.closeFiles();
    REQUIRE_FALSE (fresh.empty());
    phazite::PieceStore other (scratch);
    REQUIRE (other.write (fresh));
    other.closeFiles();
    const auto packs = scratch.findChildFiles (juce::File::findFiles, false, "*.pack");
    REQUIRE (packs.size() == 1);
    const auto blocked = versionsFolder.getChildFile ("pieces").getChildFile (packs[0].getFileName());
    REQUIRE (blocked.createDirectory());
    REQUIRE (blocked.getChildFile ("held").replaceWithText ("held"));
    return blocked;
}
} // namespace

TEST_CASE ("A merge the store can't keep the song before takes nothing, its list stays, and the view says why")
{
    WaitingMerge m;
    m.s.setVolume (m.drums, -3.0f); // so that the song before the merge has a piece the store hasn't
    REQUIRE (waitForVersionHistory (m.s.context));
    TemporaryFolder scratch;
    const auto blocked = blockPackFor (m.song().getState(), phazite::getVersionsFolder (m.song()), scratch.folder);

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

    REQUIRE (blocked.deleteRecursively());
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
// elements given), and made a branch of, as Recover Lost Commits makes one: its id.
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
    REQUIRE (phazite::recoverLostVersions (s.context) == 1);
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
        auto then = song.getState().createCopy();

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
        auto then = song.getState().createCopy();

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
        auto then = song.getState().createCopy();
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
        auto then = song().getState().createCopy();
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
    const auto before = n.song().getState().createCopy();

    REQUIRE (finished (n.s.context, [&] { return phazite::bringBackVersion (n.s.context, damaged); }));
    CHECK_FALSE (n.hasCurve()); // of a device the song hasn't: taken away again in the restore's step
    checkSong (n.song());

    // Undo brings back exactly what was; Redo the restore again.
    const auto restored = n.song().getState().createCopy();
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
