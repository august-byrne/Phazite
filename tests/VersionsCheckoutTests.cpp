// SPDX-License-Identifier: AGPL-3.0-or-later
// Checking out a commit on its own, and A/B (docs/versions.md: Checking out a commit; Hearing a version; the owner, 2026-11-12:
// "i want quick ways to be able to compare the sound between commits and also between uncommitted work"): the song made a
// commit's on no branch and back to its branch as it was left, a branch made there keeping what changed, the checkout kept as
// the project opens again; and A/B hearing the last commit's bounce in place of the song, bounced first when it has none.
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

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

// A song with Bass: its first commit at 0 dB, its second at -6 dB, and -9 dB not committed.
struct Checkout
{
    Checkout()
    {
        s.setVolume (bass, -6.0f);
        second = keepVersion (s.context, "Second");
        s.setVolume (bass, -9.0f);
    }

    float volume() { return phazite::getChannelVolume (*phazite::findSong (s.context), bass); }
    juce::String current() { return phazite::getCurrentBranch (s.context)->name; }

    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass");
    juce::String first = keepVersion (s.context, "First"), second;
};
} // namespace

TEST_CASE ("Checkout (Detached) makes the song a commit's on no branch, and Back to main brings main's song back as it was left")
{
    Checkout c;
    REQUIRE (phazite::canCheckout (c.s.context, c.first));
    REQUIRE (phazite::checkoutVersion (c.s.context, c.first, c.s.save));

    // On no branch, named as the commit is; the song as the commit had it, with no changes since.
    CHECK (phazite::isDetached (c.s.context));
    const auto current = phazite::getCurrentBranch (c.s.context);
    REQUIRE (current.has_value());
    CHECK (current->detached);
    CHECK (current->name == "First");
    CHECK (current->head == c.first);
    CHECK (c.volume() == doctest::Approx (0.0f));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (c.s.context));
    CHECK_FALSE (c.s.getProject().hasUnsavedChanges());       // the project's file is the commit's song now
    REQUIRE (phazite::getBranches (c.s.context).size() == 1); // main alone: a checkout is no branch
    CHECK (phazite::describeBackToBranch (c.s.context) == "Back to main");
    CHECK_FALSE (phazite::canDeleteBranch (c.s.context, phazite::getBranches (c.s.context)[0].id));
    CHECK_FALSE (phazite::canRenameBranch (c.s.context)); // no branch to rename: Create Branch Here names one

    // It plays and changes, but nothing is committed or merged on no branch.
    c.s.setVolume (c.bass, -3.0f);
    CHECK (phazite::hasChangesSinceLastVersion (c.s.context));
    CHECK_FALSE (phazite::hasSomethingToCommit (c.s.context));
    CHECK_FALSE (phazite::canSaveVersion (c.s.context));
    CHECK_FALSE (phazite::canUndoLastCommit (c.s.context));
    phazite::selectVersion (c.s.context, c.second);
    CHECK_FALSE (phazite::canMerge (c.s.context));

    // Back to main: its song as it was left, its change not committed too; what changed here kept in Local History first.
    const auto snapshots = phazite::getSnapshots (c.s.context).size();
    REQUIRE (phazite::canGoBackToBranch (c.s.context));
    REQUIRE (phazite::backToBranch (c.s.context, c.s.save));
    CHECK_FALSE (phazite::isDetached (c.s.context));
    CHECK (c.current() == "main");
    CHECK (c.volume() == doctest::Approx (-9.0f));
    CHECK_FALSE (c.s.getProject().hasUnsavedChanges());
    REQUIRE (waitForVersionHistory (c.s.context));
    REQUIRE (phazite::getSnapshots (c.s.context).size() == snapshots + 1);
    CHECK (phazite::getSnapshots (c.s.context).front().reason == "Before Leaving a Checkout");
    CHECK_FALSE (phazite::canGoBackToBranch (c.s.context));
}

TEST_CASE ("A commit checked out goes on to another commit checked out, or to a branch picked, and stays as the project opens again")
{
    Checkout c;
    REQUIRE (phazite::checkoutVersion (c.s.context, c.first, c.s.save));
    REQUIRE (phazite::checkoutVersion (c.s.context, c.second, c.s.save));
    CHECK (phazite::getCurrentBranch (c.s.context)->head == c.second);
    CHECK (c.volume() == doctest::Approx (-6.0f));

    // Opened again, it is where it was left: checked out, main's song kept on main.
    c.s.reopen();
    REQUIRE (waitForVersionHistory (c.s.context));
    CHECK (phazite::isDetached (c.s.context));
    CHECK (phazite::getCurrentBranch (c.s.context)->head == c.second);
    CHECK (c.volume() == doctest::Approx (-6.0f));

    // main picked in the branch picker goes there as Back to main would.
    const auto main = phazite::getBranches (c.s.context)[0].id;
    REQUIRE (phazite::switchBranch (c.s.context, main, c.s.save));
    CHECK_FALSE (phazite::isDetached (c.s.context));
    CHECK (c.volume() == doctest::Approx (-9.0f));
    CHECK (phazite::getBranches (c.s.context).size() == 1);
}

TEST_CASE ("Create Branch Here keeps what changed on a commit checked out, on a branch from it; main keeps its own song")
{
    Checkout c;
    REQUIRE (phazite::checkoutVersion (c.s.context, c.first, c.s.save));
    c.s.setVolume (c.bass, -3.0f);

    // Create Branch on the commit checked out, selected in the graph, makes it here, as the bar's Create Branch Here does.
    phazite::selectVersion (c.s.context, c.first);
    phazite::newBranchFromSelection (c.s.context, c.s.save);
    CHECK_FALSE (phazite::isDetached (c.s.context));
    CHECK (c.current() == "idea-2");
    CHECK (phazite::getCurrentBranch (c.s.context)->head == c.first);
    CHECK (c.volume() == doctest::Approx (-3.0f));
    CHECK (phazite::canSaveVersion (c.s.context)); // a commit can be made of it now
    CHECK (phazite::getBranches (c.s.context).size() == 2);

    REQUIRE (phazite::switchBranch (c.s.context, phazite::getBranches (c.s.context)[0].id, c.s.save));
    CHECK (c.current() == "main");
    CHECK (c.volume() == doctest::Approx (-9.0f));
}

TEST_CASE ("A/B hears the last commit in place of the song once it is bounced in the background, and the song at the next press")
{
    TestSong s;
    const auto track = s.addTrack ("Tone");
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Tone.wav"), 2.0, 440.0, 0.25f), 0.0);
    const auto committed = keepVersion (s.context, "Tone");

    // Nothing to compare while the song is its last commit.
    CHECK (phazite::getCompareVersion (s.context) == committed);
    CHECK_FALSE (phazite::canCompare (s.context));

    s.setVolume (track, -6.0f);
    REQUIRE (phazite::canCompare (s.context));
    CHECK (phazite::describeCompare (s.context).contains ("bounced first"));

    // A press: the commit is bounced in the background, the song playing on, and heard as soon as it is.
    phazite::toggleCompare (s.context);
    CHECK (phazite::isWaitingToCompare (s.context));
    REQUIRE (dispatchMessagesUntil ([&s, &committed] { return phazite::getHeardVersion (s.context) == committed; }, 60000));
    CHECK_FALSE (phazite::isWaitingToCompare (s.context));
    CHECK (phazite::describeHearing (s.context) == "Hearing: Tone");

    // Another: the song again. And again: the bounce, at once this time.
    phazite::toggleCompare (s.context);
    CHECK (phazite::getHeardVersion (s.context).isEmpty());
    phazite::toggleCompare (s.context);
    CHECK (phazite::getHeardVersion (s.context) == committed);

    // An edit is heard as it is made: the song again.
    s.setVolume (track, -3.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::getHeardVersion (s.context).isEmpty(); }));
    phazite::stopAndReturn (*phazite::findSong (s.context));
}

TEST_CASE ("A/B says why it can't hear a commit with nothing to bounce, until the next press or an edit")
{
    TestSong s;
    const auto track = s.addTrack ("Empty");
    keepVersion (s.context, "Nothing yet");
    s.setVolume (track, -6.0f);
    REQUIRE (phazite::canCompare (s.context));

    phazite::toggleCompare (s.context);
    REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isWaitingToCompare (s.context); }, 60000));
    CHECK (phazite::getHeardVersion (s.context).isEmpty());
    CHECK (phazite::getCompareProblem (s.context).contains ("nothing to bounce"));

    s.setVolume (track, -3.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::getCompareProblem (s.context).isEmpty(); }));
}
