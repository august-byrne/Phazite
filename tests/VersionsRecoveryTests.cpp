// SPDX-License-Identifier: AGPL-3.0-or-later
// A history that cannot lose work (docs/design/versions.md: Keeping the history whole; roadmap S4.3): what leaves every branch
// (Undo Last Version, Delete Branch, Reset Branch to Last Readable Version) is kept 30 days, listed by Recover Lost Versions
// with when and why it left, and comes back as a branch, a deleted branch with the song left on it. The days pass on the
// versions' clock, which the tests set (VersionsTime).
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "ui/versions/BranchPicker.h"

namespace
{
using namespace phazite::test;

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
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

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

const phazite::LostVersionsInfo* findLost (const std::vector<phazite::LostVersionsInfo>& lost, const juce::String& why)
{
    for (const auto& line : lost)
        if (line.why == why)
            return &line;

    return nullptr;
}

// A song with Bass on main, a branch idea-2 with a version of its own and a change left on it, and a version on main that
// was undone: idea-2 deleted. What left every branch: "Loud" (undone) and "Idea" (on idea-2, with Bass at -12 dB left on it).
struct LostWork
{
    VersionsTime clock { juce::Time::currentTimeMillis() };
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass");
    juce::String first = keepVersion (s.context, "First"), idea, loud;

    LostWork()
    {
        auto& song = *phazite::findSong (s.context);
        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        phazite::setChannelVolume (song, bass, -9.0f);
        idea = keepVersion (s.context, "Idea");
        phazite::setChannelVolume (song, bass, -12.0f); // left on it
        REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));

        phazite::setChannelVolume (*phazite::findSong (s.context), bass, -3.0f);
        loud = keepVersion (s.context, "Loud");

        // Saved on day 0; undone on day 20, idea-2 deleted on day 21, the project opened once a week meanwhile: the store's
        // sweep keeps them by the logs alone from then on, their pieces older than 30 days by then.
        openEachWeek (s, clock, 20.0);
        REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
        clock.addDays (1.0);
        REQUIRE (phazite::deleteBranch (s.context, findBranch (s.context, "idea-2")));
        REQUIRE (phazite::getBranches (s.context).size() == 1);
    }

    float volume() { return phazite::getChannelVolume (*phazite::findSong (s.context), bass); }
};
} // namespace

TEST_CASE ("An undone version and a deleted branch are listed with when and why they left, and come back 29 days later")
{
    LostWork w;

    // Listed at once, newest first, each with what it is and why it left.
    auto lost = phazite::getLostVersions (w.s.context);
    REQUIRE (lost.size() == 2);
    CHECK (lost[0].why == "delete");
    CHECK (lost[0].branch == "idea-2");
    CHECK (lost[0].lastName == "Idea");
    CHECK (lost[0].numVersions == 1);
    CHECK (lost[0].withSong);
    CHECK (lost[1].why == "undo");
    CHECK (lost[1].branch == "main");
    CHECK (lost[1].lastName == "Loud");
    CHECK_FALSE (lost[1].withSong);
    CHECK (lost[0].time.toMilliseconds() - lost[1].time.toMilliseconds() >= 24LL * 60 * 60 * 1000);

    // 29 days after the deletion (30 after the undo), the project opened once a week meanwhile: both still there, as every
    // computer's logs say, though the sweep has let go of what nothing names that is as old as their pieces.
    openEachWeek (w.s, w.clock, 29.0);
    lost = phazite::getLostVersions (w.s.context);
    REQUIRE (lost.size() == 2);
    REQUIRE (phazite::canRecoverLostVersions (w.s.context));

    // The undone version back as a branch named after main, at it.
    const auto* undone = findLost (lost, "undo");
    REQUIRE (undone != nullptr);
    const auto loudBranch = phazite::recoverLostVersions (w.s.context, undone->key);
    REQUIRE (loudBranch.isNotEmpty());
    CHECK (findBranch (w.s.context, "main (recovered)") == loudBranch);
    CHECK (phazite::getVersionHistory (w.s.context)->findVersion (w.loud) != nullptr);

    // The deleted branch back, at its last version, with the song as it was left there.
    const auto left = phazite::getLostVersions (w.s.context);
    const auto* deleted = findLost (left, "delete");
    REQUIRE (deleted != nullptr);
    const auto key = deleted->key;
    const auto ideaBranch = phazite::recoverLostVersions (w.s.context, key);
    REQUIRE (ideaBranch.isNotEmpty());
    CHECK (findBranch (w.s.context, "idea-2 (recovered)") == ideaBranch);
    CHECK (phazite::getLostVersions (w.s.context).empty());

    REQUIRE (finished (w.s.context, [&] { return phazite::switchBranch (w.s.context, ideaBranch, w.s.save); }));
    CHECK (phazite::getLastVersion (w.s.context)->id == w.idea);
    CHECK (w.volume() == doctest::Approx (-12.0f));

    REQUIRE (finished (w.s.context, [&] { return phazite::switchBranch (w.s.context, loudBranch, w.s.save); }));
    CHECK (phazite::getLastVersion (w.s.context)->id == w.loud);
    CHECK (w.volume() == doctest::Approx (-3.0f));

    // Recovered, they are on branches again, and so on every computer as the project opens.
    w.s.reopen();
    REQUIRE (waitForVersionHistory (w.s.context));
    CHECK (phazite::getLostVersions (w.s.context).empty());
    CHECK (phazite::getBranches (w.s.context).size() == 3);
}

TEST_CASE ("What left every branch more than 30 days ago is no longer offered, and the store lets it go")
{
    SweepLetsGo letGo; // off in the product: there it is no longer offered, and stays in the store
    LostWork w;
    openEachWeek (w.s, w.clock, 38.0); // swept on day 56: the undo 36 days gone, the deletion 35
    CHECK (phazite::getLostVersions (w.s.context).empty());
    CHECK_FALSE (phazite::canRecoverLostVersions (w.s.context));

    // And the store has let them go, the song left on idea-2 with them.
    const auto store = phazite::getVersionHistory (w.s.context)->getStore();
    CHECK_FALSE (store->getPieces().has (w.loud));
    CHECK_FALSE (store->getPieces().has (w.idea));
}

TEST_CASE ("A branch recovered and deleted again is listed once, as its newest deletion")
{
    LostWork w;
    const auto lost = phazite::getLostVersions (w.s.context);
    const auto* deleted = findLost (lost, "delete");
    REQUIRE (deleted != nullptr);
    const auto recovered = phazite::recoverLostVersions (w.s.context, deleted->key);
    REQUIRE (recovered.isNotEmpty());
    w.clock.addDays (1.0);
    REQUIRE (phazite::deleteBranch (w.s.context, recovered));

    const auto again = phazite::getLostVersions (w.s.context);
    CHECK (std::count_if (again.begin(), again.end(), [] (const phazite::LostVersionsInfo& line) { return line.lastName == "Idea"; }) == 1);
    const auto* newest = findLost (again, "delete");
    REQUIRE (newest != nullptr);
    CHECK (newest->branch == "idea-2 (recovered)");
}

TEST_CASE ("A merge waiting whose song before it is no longer kept says so")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    phazite::setChannelVolume (song, bass, -9.0f);
    const auto theirs = keepVersion (s.context, "Idea");
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));

    // A merge of idea-2 waiting, as a song saved long ago holds it, the song before it gone from the store.
    juce::ValueTree merging (phazite::schema::MERGING);
    merging.setProperty (phazite::schema::mergingVersion, theirs, nullptr);
    merging.setProperty (phazite::schema::mergingHead, phazite::getLastVersion (s.context)->id, nullptr);
    merging.setProperty (phazite::schema::mergingBefore, juce::String::repeatedString ("c", 64), nullptr);
    phazite::findSong (s.context)->getState().appendChild (merging, nullptr);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::getMergeProblem (s.context).startsWith ("Reading"); }));
    CHECK (phazite::getMergeProblem (s.context).contains ("no longer kept"));
}

TEST_CASE ("A deleted branch whose versions another branch holds comes back for the changes left on it")
{
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);

    REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    phazite::setChannelVolume (song, bass, -7.0f); // a change left on idea-2, no version of its own
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    REQUIRE (waitForVersionHistory (s.context));

    // Its question says what goes, and for how long it can come back, once what was left there is read (known bug 126).
    phazite::describeBranchDeletion (s.context, findBranch (s.context, "idea-2"));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::describeBranchDeletion (s.context, findBranch (s.context, "idea-2"))
           == "This also deletes the changes left on it. Recover Lost Versions brings them back for 30 days.");
    REQUIRE (phazite::deleteBranch (s.context, findBranch (s.context, "idea-2")));

    const auto lost = phazite::getLostVersions (s.context);
    REQUIRE (lost.size() == 1);
    CHECK (lost[0].why == "delete");
    CHECK (lost[0].numVersions == 0);
    CHECK (lost[0].withSong);

    const auto branch = phazite::recoverLostVersions (s.context, lost[0].key);
    REQUIRE (branch.isNotEmpty());
    CHECK (phazite::getLostVersions (s.context).empty());
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, branch, s.save); }));
    CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-7.0f));
}

TEST_CASE ("A branch a command made and took back leaves nothing to recover")
{
    TestSong s;
    s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    auto* history = phazite::getVersionHistory (s.context);

    // As Create Branch from a version that was cancelled takes its branch away again (VersionsModel.cpp: newBranch()).
    const auto made = history->makeBranch (first, false);
    REQUIRE (made.isNotEmpty());
    REQUIRE (history->deleteBranch (made, true));
    CHECK (phazite::getLostVersions (s.context).empty());
}

TEST_CASE ("Another computer's deletion is listed here too, by its log")
{
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);

    // Another computer saved a version on a branch of its own and deleted it: its records and its log say so.
    juce::String theirs;

    {
        phazite::VersionStore elsewhere (phazite::getVersionsFolder (song), "elsewhere");
        phazite::VersionRecord record;
        record.name = "Theirs";
        record.parents.add (first);
        record.branch = "their-branch";
        record.saved = clock.get();
        theirs = elsewhere.writeVersion (record, phazite::splitSong (phazite::test::copyWholeSong (song)));
        REQUIRE (theirs.isNotEmpty());

        phazite::BranchRecord branch;
        branch.id = "their-branch";
        branch.name = "Their idea";
        branch.computer = "elsewhere";
        branch.head = theirs;
        branch.changed = clock.get();
        branch.deleted = true;
        REQUIRE (elsewhere.writeBranch (branch));
        REQUIRE (elsewhere.logBranchMove (branch.id, {}, theirs, "commit", branch.name));
        REQUIRE (elsewhere.logBranchMove (branch.id, theirs, {}, "delete", branch.name));
        elsewhere.getPieces().closeFiles();
    }

    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    const auto lost = phazite::getLostVersions (s.context);
    REQUIRE (lost.size() == 1);
    CHECK (lost[0].branch == "Their idea");
    CHECK (lost[0].lastName == "Theirs");
    REQUIRE (phazite::recoverLostVersions (s.context, lost[0].key).isNotEmpty());
    CHECK (findBranch (s.context, "Their idea (recovered)").isNotEmpty());
}

TEST_CASE ("When something left is said as the history says when a version was saved")
{
    using phazite::ui::BranchPicker;
    const auto now = juce::Time (2026, 10, 17, 12, 0); // 17 Nov 2026, a Tuesday
    CHECK (BranchPicker::describeOn (now - juce::RelativeTime::seconds (20.0), now) == "just now");
    CHECK (BranchPicker::describeOn (now - juce::RelativeTime::hours (3.0), now) == "3 h ago");
    CHECK (BranchPicker::describeOn (now - juce::RelativeTime::days (3.0), now) == "on Sat");
    CHECK (BranchPicker::describeOn (now - juce::RelativeTime::days (12.0), now) == "on 5 Nov");

    phazite::LostVersionsInfo deleted { "k", "idea-3", "delete", now - juce::RelativeTime::days (3.0), 2, "Verse", true };
    CHECK (BranchPicker::describeLost (deleted, now) == "On idea-3, deleted on Sat, 2 versions, with the changes left on it");
    phazite::LostVersionsInfo undone { "k", "main", "undo", now - juce::RelativeTime::days (12.0), 1, "Verse", false };
    CHECK (BranchPicker::describeLost (undone, now) == "Undone on 5 Nov, from main");
}
