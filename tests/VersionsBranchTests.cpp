// SPDX-License-Identifier: AGPL-3.0-or-later
// Branches and merges of a project's versions (docs/versions.md: Branches, Merging; step V4): through the model, as the
// Versions view and its commands make them, with the project opened again on each switch as the app opens it.
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

namespace
{
using namespace phazite::test;

void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isBusy();
        }));
}

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name = {})
{
    waitForVersions (context);
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }));
    REQUIRE (result->wasOk());
    waitForVersions (context);
    return phazite::getLastVersion (context)->id;
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

// A song with Bass and Drums, its first version on Main, and the way the app opens another branch's song.
struct Branches
{
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass"), drums = s.addTrack ("Drums");
    juce::String first = keepVersion (s.context, "First");
    phazite::OpenSong open = [this] (const juce::ValueTree& song)
    {
        s.openSong (song);
        waitForVersions (s.context);
    };

    float volume (phazite::TrackID track) { return phazite::getChannelVolume (*phazite::findSong (s.context), track); }
    juce::String current() { return phazite::getCurrentBranch (s.context)->name; }
};
} // namespace

TEST_CASE ("New Branch from Now goes on with the song and its changes on a branch named Idea 2, in a color of its own")
{
    Branches b;
    b.s.setVolume (b.bass, -6.0f);
    REQUIRE (phazite::newBranch (b.s.context, {}, b.open));

    CHECK (b.current() == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    CHECK (phazite::hasChangesSinceLastVersion (b.s.context)); // still since the version it was made from
    const auto branches = phazite::getBranches (b.s.context);
    REQUIRE (branches.size() == 2);
    CHECK (branches[0].name == "Main");
    CHECK (branches[1].head == b.first);
    CHECK (branches[0].colour != branches[1].colour);

    // The next one is Idea 3.
    REQUIRE (phazite::newBranch (b.s.context, {}, b.open));
    CHECK (b.current() == "Idea 3");
}

TEST_CASE ("Switching keeps each branch's song as it was left there, unsaved changes included, and starts the undo history again")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.open));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter bass");
    b.s.setVolume (b.bass, -9.0f); // not in a version

    // Main as its last version had it.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.open));
    CHECK (b.current() == "Main");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK_FALSE (phazite::canUndo (*phazite::findSong (b.s.context)));
    CHECK_FALSE (b.s.getProject().hasUnsavedChanges()); // the project file is Main's song now

    b.s.addTrack ("Keys"); // left on Main

    // Idea 2 as it was left, its change since its version too.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Idea 2"), b.open));
    CHECK (b.current() == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f));
    CHECK (phazite::getChangesSinceLastVersion (b.s.context).size() == 1);
    CHECK (phazite::getTrackIDs (*phazite::findSong (b.s.context)).size() == 3);

    // And Main with Keys.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.open));
    CHECK (phazite::getTrackIDs (*phazite::findSong (b.s.context)).size() == 4);

    // It opens again on the branch it was on.
    b.s.reopen();
    waitForVersions (b.s.context);
    CHECK (b.current() == "Main");
}

TEST_CASE ("New Branch from a version opens that version's song on the new branch")
{
    Branches b;
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Second");
    b.s.setVolume (b.bass, -12.0f);

    REQUIRE (phazite::newBranch (b.s.context, b.first, b.open));
    CHECK (b.current() == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK (phazite::getLastVersion (b.s.context)->id == b.first);

    // Main kept its change since its last version.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.open));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("A branch that changed other tracks merges with no clashes, and the version after follows both lines")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.open));
    b.s.setVolume (b.drums, -9.0f);
    const auto drumsVersion = keepVersion (b.s.context, "Drums down");

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.open));
    b.s.setVolume (b.bass, -6.0f);
    const auto bassVersion = keepVersion (b.s.context, "Bass down");

    // The version the current branch has can't be merged; the other's can.
    phazite::selectVersion (b.s.context, b.first);
    CHECK_FALSE (phazite::canMerge (b.s.context));
    phazite::selectVersion (b.s.context, drumsVersion);
    REQUIRE (phazite::canMerge (b.s.context));

    REQUIRE (phazite::startMerge (b.s.context, drumsVersion));
    CHECK (phazite::describeMerge (b.s.context) == "Drums down");
    const auto lines = phazite::getMergeLines (b.s.context);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0].change.name == "Drums");
    CHECK_FALSE (lines[0].clashes);
    CHECK (lines[0].ticked);

    phazite::finishTickedMerge (b.s.context);
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    CHECK (b.volume (b.drums) == doctest::Approx (-9.0f));
    CHECK (phazite::describeUndo (phazite::findSong (b.s.context)) == "Undo Merge \"Drums down\"");
    CHECK (phazite::isMerging (b.s.context));
    CHECK (phazite::getClashes (b.s.context).empty());

    // The version that finishes it follows both, and is named for what it merged.
    const auto merged = keepVersion (b.s.context);
    CHECK_FALSE (phazite::isMerging (b.s.context));
    const auto version = phazite::getLastVersion (b.s.context);
    CHECK (version->parents == juce::StringArray (bassVersion, drumsVersion));
    CHECK (version->name == "Merge \"Drums down\"");
    juce::ignoreUnused (merged);

    // Merged once, the other's version is in the current branch's past.
    phazite::selectVersion (b.s.context, drumsVersion);
    CHECK_FALSE (phazite::canMerge (b.s.context));
}

TEST_CASE ("A knob both branches changed is a clash, which switches between its three values, each an undo step")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.open));
    b.s.setVolume (b.bass, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Much quieter");

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.open));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter");

    REQUIRE (phazite::startMerge (b.s.context, theirs));
    const auto lines = phazite::getMergeLines (b.s.context);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0].clashes);

    // Ticked off, nothing is taken.
    phazite::tickMergeLine (b.s.context, 0, false);
    CHECK_FALSE (phazite::getMergeLines (b.s.context)[0].ticked);
    phazite::tickMergeLine (b.s.context, 0, true);
    phazite::finishTickedMerge (b.s.context);

    const auto clashes = phazite::getClashes (b.s.context);
    REQUIRE (clashes.size() == 1);
    CHECK (clashes[0].line == "Bass");
    CHECK (clashes[0].what == "volume");
    CHECK (clashes[0].chosen == 1);
    CHECK (clashes[0].otherBranch == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f)); // this branch's, to start with

    REQUIRE (phazite::chooseClash (b.s.context, 0, 2));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
    CHECK (phazite::describeUndo (phazite::findSong (b.s.context)) == "Undo Take Idea 2's Bass volume");
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 2);

    REQUIRE (phazite::chooseClash (b.s.context, 0, 0));
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f)); // as before either changed it

    phazite::undo (*phazite::findSong (b.s.context));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("The branch commands act on the version and the branch the Versions view picks")
{
    Branches b;
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, b.s.context, [] (std::function<void (bool)> then) { then (true); }, b.open);

    phazite::selectVersion (b.s.context, {}); // Now
    REQUIRE (commands.invoke ("version.newBranch"));
    CHECK (b.current() == "Idea 2");

    phazite::selectBranch (b.s.context, findBranch (b.s.context, "Main"));
    REQUIRE (commands.invoke ("version.switchBranch"));
    CHECK (b.current() == "Main");
    CHECK (phazite::getCurrentName (*commands.find ("version.merge")) == "Merge into Main...");
    CHECK_FALSE (commands.invoke ("version.finishMerge")); // no merge's list shows
}
