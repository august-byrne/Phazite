// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's history on two computers sharing a synced folder (docs/design/versions.md: The store), for when the sweep lets
// go (off in the product: model/versions/internal/StoreSweep.h, sweepLetsGo; the tests turn it on with SweepLetsGo): each
// computer keeps its own copy of every version and song its history names (adoptVersions()), so that the other's sweep of
// its own packs never takes what this one merged, saved on top of, branched from, undid back onto or recovered. And what
// the history says when it can't be sure: a version whose earlier one can't be read, a sweep that kept everything, a clock
// set back, an audio file that can't be read, Verify stopped by a save or a read.
#include "HeldFile.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/StoreSweep.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

namespace
{
using namespace phazite::test;

// The current branch's last version, and the current branch, failing the test where there is none.
juce::String lastVersion (const phazite::ProjectContext& context)
{
    const auto last = phazite::getLastVersion (context);
    REQUIRE (last.has_value());
    return last->id;
}

juce::String currentBranch (const phazite::ProjectContext& context)
{
    const auto branch = phazite::getCurrentBranch (context);
    REQUIRE (branch.has_value());
    return branch->id;
}

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
    return lastVersion (context);
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

void verify (phazite::ProjectContext& context)
{
    REQUIRE (phazite::verifyHistory (context));
    REQUIRE (dispatchMessagesUntil ([&context] { return ! phazite::isVerifyingHistory (context); }, versionHistoryTimeout));
}

// This computer (A) with a project, and another computer (B) that saved a version on top of A's first, on a branch of its
// own, into the same synced folder; then what B's sweep of its own packs does on A's disk once the sync tool carries it
// there: B's packs gone.
struct TwoComputers
{
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass");
    juce::String first = keepVersion (s.context, "First");
    juce::StringArray bPacks;

    juce::File getFolder() { return phazite::getVersionsFolder (*phazite::findSong (s.context)); }

    static void renameBass (juce::ValueTree tree)
    {
        if (tree.hasType (phazite::schema::TRACK) && tree[phazite::schema::trackName].toString() == "Bass")
            tree.setProperty (phazite::schema::trackName, "Bass on B", nullptr);

        for (auto child : tree)
            renameBass (child);
    }

    static bool hasBassOnB (const juce::ValueTree& tree)
    {
        if (tree.hasType (phazite::schema::TRACK) && tree[phazite::schema::trackName].toString() == "Bass on B")
            return true;

        for (const auto& child : tree)
            if (hasBassOnB (child))
                return true;

        return false;
    }

    bool songIsBs() { return hasBassOnB (phazite::findSong (s.context)->getState()); }

    // B's version on a branch of B's, "theirs": A's first with Bass renamed "Bass on B", seen by A as it opens again.
    juce::String saveOnB()
    {
        const auto before = getFolder().getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack");
        juce::String id;

        {
            phazite::VersionStore b (getFolder(), "computer-b");
            auto song = phazite::test::copyWholeSong (*phazite::findSong (s.context));
            renameBass (song);
            phazite::VersionRecord record;
            record.name = "B's";
            record.parents.add (first);
            record.branch = "theirs";
            record.saved = juce::Time::currentTimeMillis();
            id = b.writeVersion (record, phazite::splitSong (song));
            REQUIRE (id.isNotEmpty());

            phazite::BranchRecord theirs;
            theirs.id = "theirs";
            theirs.name = "theirs";
            theirs.head = id;
            theirs.computer = "computer-b";
            theirs.changed = juce::Time::currentTimeMillis();
            REQUIRE (b.writeBranch (theirs));
            b.getPieces().closeFiles();
        }

        for (const auto& pack : getFolder().getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"))
            if (! before.contains (pack))
                bPacks.add (pack.getFullPathName());

        s.reopen();
        REQUIRE (waitForVersionHistory (s.context));
        return id;
    }

    // B swept its own packs (its versions left its branches more than 30 days ago), and the sync carried that here.
    void bSweepsItsPacks()
    {
        REQUIRE (waitForVersionHistory (s.context));
        phazite::getVersionHistory (s.context)->getStore()->getPieces().closeFiles();

        for (const auto& path : bPacks)
            REQUIRE (juce::File (path).deleteFile());

        s.reopen();
        REQUIRE (waitForVersionHistory (s.context));
    }

    bool reads (const juce::String& version)
    {
        auto* history = phazite::getVersionHistory (s.context);
        return history->findVersion (version) != nullptr && history->readSong (version).isValid();
    }
};
} // namespace

TEST_CASE ("A merge of another computer's branch stays whole after that computer swept the branch away")
{
    SweepLetsGo letGo;
    TwoComputers t;
    t.s.setVolume (t.bass, -4.0f);
    const auto second = keepVersion (t.s.context, "Second"); // main moved on too: a merge, not a fast-forward
    const auto theirs = t.saveOnB();
    REQUIRE (finished (t.s.context, [&] { return phazite::mergeBranch (t.s.context, "theirs"); }));
    REQUIRE (phazite::finishMerge (t.s.context));
    REQUIRE (phazite::isMerging (t.s.context));
    REQUIRE (t.songIsBs());
    REQUIRE (waitForVersionHistory (t.s.context));

    SUBCASE ("The merge waiting still reads")
    {
        t.bSweepsItsPacks();
        REQUIRE (phazite::isMerging (t.s.context));
        REQUIRE (dispatchMessagesUntil ([&t] { return ! phazite::getMergeProblem (t.s.context).startsWith ("Reading"); }));
        CHECK (phazite::getMergeProblem (t.s.context).isEmpty());
        CHECK (t.reads (theirs));
        CHECK (t.reads (second));
    }

    SUBCASE ("The merge's version, and the line it merged in")
    {
        const auto merged = keepVersion (t.s.context, "Merged");
        t.bSweepsItsPacks();
        CHECK (t.reads (merged));
        CHECK (t.reads (theirs));
        const auto* record = phazite::getVersionHistory (t.s.context)->findVersion (merged);
        REQUIRE (record != nullptr);
        CHECK (record->parents.contains (theirs));
        verify (t.s.context);
        CHECK (phazite::describeHistoryCheck (t.s.context).contains ("all whole"));
    }
}

TEST_CASE ("A version saved on top of another computer's, and an undo back onto it, stay whole after that computer swept")
{
    SweepLetsGo letGo;
    TwoComputers t;
    const auto theirs = t.saveOnB();

    // Their branch switched to, a version saved on top of theirs, and undone back onto it; then B's sweep.
    REQUIRE (finished (t.s.context, [&] { return phazite::switchBranch (t.s.context, "theirs", t.s.save); }));
    REQUIRE (t.songIsBs());
    t.s.setVolume (t.bass, -4.0f);
    const auto mine = keepVersion (t.s.context, "Mine");

    SUBCASE ("Saved on top")
    {
        t.bSweepsItsPacks();
        CHECK (t.reads (mine));
        CHECK (t.reads (theirs));
        verify (t.s.context);
        CHECK (phazite::describeHistoryCheck (t.s.context).contains ("all whole"));
    }

    SUBCASE ("Undone back onto it")
    {
        REQUIRE (finished (t.s.context, [&] { return phazite::undoLastCommit (t.s.context); }));
        REQUIRE (lastVersion (t.s.context) == theirs);
        t.bSweepsItsPacks();
        CHECK (lastVersion (t.s.context) == theirs);
        CHECK (t.reads (theirs));
        verify (t.s.context);
        CHECK (phazite::describeHistoryCheck (t.s.context).contains ("all whole"));
    }
}

TEST_CASE ("A branch made from another computer's version stays whole after that computer swept")
{
    SweepLetsGo letGo;
    TwoComputers t;
    const auto theirs = t.saveOnB();
    REQUIRE (finished (t.s.context, [&] { return phazite::newBranch (t.s.context, theirs, t.s.save); }));
    const auto idea = currentBranch (t.s.context);
    REQUIRE (idea != "theirs");
    REQUIRE (finished (t.s.context, [&] { return phazite::switchBranch (t.s.context, findBranch (t.s.context, "main"), t.s.save); }));
    REQUIRE_FALSE (t.songIsBs());
    t.bSweepsItsPacks();

    REQUIRE (finished (t.s.context, [&] { return phazite::switchBranch (t.s.context, idea, t.s.save); }));
    CHECK (lastVersion (t.s.context) == theirs);
    CHECK (t.songIsBs());
}

TEST_CASE ("Verify History names a version whose earlier version can't be read")
{
    SweepLetsGo letGo;
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);

    {
        phazite::VersionStore store (phazite::getVersionsFolder (song), phazite::getThisComputer());
        phazite::VersionRecord record;
        record.name = "Orphan";
        record.parents.add (juce::String::repeatedString ("e", 64)); // a version not in the folder
        record.branch = currentBranch (s.context);
        record.saved = juce::Time::currentTimeMillis();
        const auto id = store.writeVersion (record, phazite::splitSong (song.getState()));
        REQUIRE (id.isNotEmpty());
        auto main = *phazite::getVersionHistory (s.context)->findCurrentBranch();
        main.head = id;
        REQUIRE (store.writeBranch (main));
    }

    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    verify (s.context);
    CHECK (phazite::describeHistoryCheck (s.context).contains ("\"Orphan\" follows a version that can't be read"));
    CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("all whole"));
}

TEST_CASE ("A sweep that couldn't be sure says so where Verify History says what it found")
{
    SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "First");
    openEachWeek (s, clock, 28.0);

    // A file that names what is kept, damaged: the next sweep keeps everything, and says why.
    const auto local = phazite::getVersionHistory (s.context)->getStore()->getLocalFolder();
    REQUIRE (local.getChildFile ("working.xml").replaceWithText ("<WORKING><LEFT branch=\"main\" song="));
    openEachWeek (s, clock, 7.0);
    CHECK (phazite::describeHistoryCheck (s.context).startsWith ("Phazite is keeping everything in the Versions folder, because "));
}

TEST_CASE ("A clock set back at every start stamps nothing older than what was stamped before it, a sweep between or not")
{
    SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    const auto* firstRecord = phazite::getVersionHistory (s.context)->findVersion (first);
    REQUIRE (firstRecord != nullptr);
    const auto firstSaved = firstRecord->saved;
    openEachWeek (s, clock, 7.0); // a sweep noted

    // A dead battery, which resets the clock at every start: a year behind for a session (whose sweep writes its noted time
    // again as that now), and again for another, eight days of it later, whose sweep is due; there a version is undone.
    const auto right = clock.get();
    const auto behind = right - 365LL * 24 * 60 * 60 * 1000;
    clock.set (behind);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    clock.set (behind + 8LL * 24 * 60 * 60 * 1000);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (phazite::getVersionHistory (s.context)->getStore()->readSwept().time == clock.get());
    s.setVolume (bass, -6.0f);
    keepVersion (s.context, "Undone");
    REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));

    // Put right: the undone version is listed as left no earlier than what was saved before it, not a year ago.
    clock.set (right + 24LL * 60 * 60 * 1000);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    const auto lost = phazite::getLostVersions (s.context);
    REQUIRE (lost.size() == 1);
    CHECK (lost[0].lastName == "Undone");
    CHECK (lost[0].time.toMilliseconds() > firstSaved);

    // Weeks on, the clock right, it is still there, and in the store: its 30 days count from when it was undone.
    openEachWeek (s, clock, 14.0);
    CHECK (phazite::getLostVersions (s.context).size() == 1);
    CHECK (phazite::canRecoverLostVersions (s.context));
}

TEST_CASE ("Snapshots taken while a clock set back holds the stamps up each have a time of their own, and restore their own song")
{
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    openEachWeek (s, clock, 7.0);
    clock.set (clock.get() - 365LL * 24 * 60 * 60 * 1000); // and stays there: every stamp of the session is the floor's
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));

    for (const auto volume : { -11.0f, -12.0f, -13.0f })
    {
        s.setVolume (bass, volume);
        phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
        REQUIRE (waitForVersionHistory (s.context));
    }

    std::vector<juce::int64> times;

    for (const auto& snapshot : phazite::getSnapshots (s.context))
        if (snapshot.reason == "Auto-saved")
            times.push_back (snapshot.time);

    REQUIRE (times.size() == 3);
    CHECK (times[0] > times[1]);
    CHECK (times[1] > times[2]);

    // Newest first: -13, -12, -11 dB.
    for (size_t i = 0; i < times.size(); ++i)
    {
        REQUIRE (finished (s.context, [&] { return phazite::restoreSnapshot (s.context, times[i]); }));
        CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-13.0f + (float) i));
    }
}

TEST_CASE ("Verify History says an audio file that can't be read, and never calls the history whole when it was stopped")
{
    SweepLetsGo letGo;
    TestSong s;
    const auto track = s.addTrack ("Drums");
    const auto clip = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"), 0.25), 0.0);
    keepVersion (s.context, "Drums");

    SUBCASE ("A file another program holds")
    {
        // A file the song plays is open in Phazite itself, which on Windows keeps another program from holding it (no one
        // else may then, so it isn't held at all): the version alone plays it here, the project opened again without it.
        s.select (clip);
        s.run ("edit.delete");
        s.reopen();
        REQUIRE (waitForVersionHistory (s.context));
        const HeldFile held (s.getProject().getAudioFolder().getChildFile ("Kick.wav"));

        if (! held.works())
        {
            MESSAGE ("The file can't be held from this user: nothing to fail.");
            return;
        }

        verify (s.context);
        CHECK (phazite::describeHistoryCheck (s.context).contains ("Audio/Kick.wav can't be read"));
    }

    SUBCASE ("Stopped by a save")
    {
        phazite::VersionHistory::holdReadsForTests = true;
        REQUIRE (phazite::verifyHistory (s.context));
        s.setVolume (track, -3.0f);
        phazite::setVersionDraft (s.context, "Meanwhile", {});
        std::optional<juce::Result> result;
        REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
        phazite::VersionHistory::holdReadsForTests = false;
        REQUIRE (dispatchMessagesUntil ([&] { return result.has_value() && ! phazite::isVerifyingHistory (s.context); }));
        CHECK (result->wasOk());
        CHECK (phazite::describeHistoryCheck (s.context).contains ("stopped before it was done"));
        CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    }

    SUBCASE ("Stopped by a command that reads songs")
    {
        const auto drums = lastVersion (s.context);
        s.setVolume (track, -3.0f);
        keepVersion (s.context, "Quieter"); // so that Drums, no longer the last, is read to compare with
        phazite::VersionHistory::holdReadsForTests = true;
        REQUIRE (phazite::verifyHistory (s.context));
        REQUIRE (phazite::compareWithVersion (s.context, drums));
        phazite::VersionHistory::holdReadsForTests = false;
        REQUIRE (waitForVersionHistory (s.context));
        REQUIRE (dispatchMessagesUntil ([&] { return ! phazite::isVerifyingHistory (s.context); }));
        CHECK (phazite::describeHistoryCheck (s.context).contains ("stopped before it was done"));
        CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    }
}

TEST_CASE ("A line Verify History found that the store let go of since comes back as nothing, and the view says why")
{
    SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    juce::String lostID;

    // A version of this computer's that no record or log names (a record lost), its pieces 40 days old by now.
    {
        phazite::VersionStore here (phazite::getVersionsFolder (song), phazite::getThisComputer());
        phazite::VersionRecord record;
        record.name = "Lost";
        record.saved = clock.get();
        auto copy = phazite::test::copyWholeSong (song);
        copy.setProperty ("madeHere", true, nullptr);
        lostID = here.writeVersion (record, phazite::splitSong (copy));
        REQUIRE (lostID.isNotEmpty());
        here.getPieces().closeFiles();
    }

    clock.addDays (40.0);
    verify (s.context);
    const auto lost = phazite::getLostVersions (s.context);
    REQUIRE (lost.size() == 1);
    REQUIRE (lost[0].why == "found");

    // The store lets it go meanwhile, as a sweep does (nothing names it, and it is older than 30 days).
    const auto store = phazite::getVersionHistory (s.context)->getStore();
    phazite::sweepStore (*store, {}, 50, [] { return false; });
    phazite::sweepStore (*store, {}, 50, [] { return false; }, true);
    REQUIRE_FALSE (store->getPieces().has (lostID));

    CHECK (phazite::recoverLostVersions (s.context, lost[0].key).isEmpty());
    CHECK (phazite::getReadingProblem (s.context).contains ("no longer kept"));
    CHECK (phazite::getBranches (s.context).size() == 1);
}

TEST_CASE ("A song another computer left on a branch it deleted, recovered here, stays whole after that computer swept")
{
    SweepLetsGo letGo;
    TwoComputers t;

    // 1. B saves a version on a branch of its own and leaves a change on it ("Bass on B"); 2. B deletes the branch, its log
    // naming the song left there.
    juce::String leftSong;

    {
        const auto before = t.getFolder().getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack");
        phazite::VersionStore b (t.getFolder(), "computer-b");
        phazite::VersionRecord record;
        record.name = "B's";
        record.parents.add (t.first);
        record.branch = "theirs";
        record.saved = juce::Time::currentTimeMillis();
        const auto head = b.writeVersion (record, phazite::splitSong (phazite::findSong (t.s.context)->getState()));
        REQUIRE (head.isNotEmpty());

        auto left = phazite::test::copyWholeSong (*phazite::findSong (t.s.context));
        TwoComputers::renameBass (left);
        const auto pieces = phazite::splitSong (left);
        REQUIRE (b.writeSong (pieces));
        leftSong = pieces.song;

        phazite::BranchRecord theirs;
        theirs.id = "theirs";
        theirs.name = "Their idea";
        theirs.head = head;
        theirs.computer = "computer-b";
        theirs.changed = juce::Time::currentTimeMillis();
        theirs.deleted = true;
        REQUIRE (b.writeBranch (theirs));
        REQUIRE (b.logBranchMove (theirs.id, {}, head, "commit", theirs.name));
        REQUIRE (b.logBranchMove (theirs.id, head, {}, "delete", theirs.name, leftSong));
        b.getPieces().closeFiles();

        for (const auto& pack : t.getFolder().getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"))
            if (! before.contains (pack))
                t.bPacks.add (pack.getFullPathName());
    }

    // 3. This computer opens the project and recovers the line, with the song left on it.
    t.s.reopen();
    REQUIRE (waitForVersionHistory (t.s.context));
    const auto lost = phazite::getLostVersions (t.s.context);
    REQUIRE (lost.size() == 1);
    REQUIRE (lost[0].withSong);
    const auto recovered = phazite::recoverLostVersions (t.s.context, lost[0].key);
    REQUIRE (recovered.isNotEmpty());
    REQUIRE (waitForVersionHistory (t.s.context));

    // 4. B's sweep takes its packs away; 5. the recovered branch switched to: the song as B left it.
    t.bSweepsItsPacks();
    REQUIRE (finished (t.s.context, [&] { return phazite::switchBranch (t.s.context, recovered, t.s.save); }));
    CHECK (t.songIsBs());
}

TEST_CASE ("A command's own copy keeps another computer's version whole, with no copy made as the history opens")
{
    SweepLetsGo letGo;
    struct NoCopyAtOpen
    {
        NoCopyAtOpen() { phazite::VersionHistory::noAdoptionAtOpenForTests = true; }
        ~NoCopyAtOpen() { phazite::VersionHistory::noAdoptionAtOpenForTests = false; }
    } noCopyAtOpen;

    TwoComputers t;
    const auto theirs = t.saveOnB();
    REQUIRE_FALSE (phazite::getVersionHistory (t.s.context)->getStore()->getPieces().hasOwnCopy (theirs));

    // The switch to B's branch copies its version here.
    REQUIRE (finished (t.s.context, [&] { return phazite::switchBranch (t.s.context, "theirs", t.s.save); }));
    REQUIRE (waitForVersionHistory (t.s.context));
    CHECK (phazite::getVersionHistory (t.s.context)->getStore()->getPieces().hasOwnCopy (theirs));

    t.bSweepsItsPacks();
    CHECK (t.reads (theirs));
}

TEST_CASE ("A floor for the stamps from a damaged file, or from a time thousands of years ahead, is passed over")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    const auto local = phazite::getVersionHistory (s.context)->getStore()->getLocalFolder();

    SUBCASE ("The largest number there is, in the file that keeps the floor")
    {
        REQUIRE (local.getChildFile ("stamped").replaceWithText ("9223372036854775807\n"));
    }

    SUBCASE ("A log's line dated in the year 9999")
    {
        const auto log = local.getChildFile ("logs").getChildFile (currentBranch (s.context));
        REQUIRE (log.appendText ("253402300799000 " + first + " " + first + " commit - main\n", false, false));
    }

    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    s.setVolume (bass, -5.0f);
    const auto second = keepVersion (s.context, "Second");
    const auto* record = phazite::getVersionHistory (s.context)->findVersion (second);
    REQUIRE (record != nullptr);
    CHECK (record->saved < juce::Time::currentTimeMillis() + 24LL * 60 * 60 * 1000);
    CHECK (record->saved > phazite::getVersionHistory (s.context)->findVersion (first)->saved);
}
