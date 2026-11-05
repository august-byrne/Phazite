// SPDX-License-Identifier: AGPL-3.0-or-later
// Branches and merges of a project's versions (docs/versions.md: Branches, Merging; step V4): through the model, as the
// Versions view and its commands make them, each switch making the song the other branch's in place.
#include "EnginePlayback.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
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

// A song with Bass and Drums, and its first version on Main.
struct Branches
{
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass"), drums = s.addTrack ("Drums");
    juce::String first = keepVersion (s.context, "First");

    float volume (phazite::TrackID track) { return phazite::getChannelVolume (*phazite::findSong (s.context), track); }
    juce::String current() { return phazite::getCurrentBranch (s.context)->name; }
};
} // namespace

TEST_CASE ("New Branch from Now goes on with the song and its changes on a branch named Idea 2, in a color of its own")
{
    Branches b;
    b.s.setVolume (b.bass, -6.0f);
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));

    CHECK (b.current() == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    CHECK (phazite::hasChangesSinceLastVersion (b.s.context)); // still since the version it was made from
    const auto branches = phazite::getBranches (b.s.context);
    REQUIRE (branches.size() == 2);
    CHECK (branches[0].name == "Main");
    CHECK (branches[1].head == b.first);
    CHECK (branches[0].colour != branches[1].colour);

    // The next one is Idea 3.
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    CHECK (b.current() == "Idea 3");
}

TEST_CASE ("Switching keeps each branch's song as it was left there, unsaved changes included, and starts the undo history again")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter bass");
    b.s.setVolume (b.bass, -9.0f); // not in a version

    // Main as its last version had it.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK (b.current() == "Main");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK_FALSE (phazite::canUndo (*phazite::findSong (b.s.context)));
    CHECK_FALSE (b.s.getProject().hasUnsavedChanges()); // the project file is Main's song now

    b.s.addTrack ("Keys"); // left on Main

    // Idea 2 as it was left, its change since its version too.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Idea 2"), b.s.save));
    CHECK (b.current() == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f));
    CHECK (phazite::getChangesSinceLastVersion (b.s.context).size() == 1);
    CHECK (phazite::getTrackIDs (*phazite::findSong (b.s.context)).size() == 3);

    // And Main with Keys.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
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

    REQUIRE (phazite::newBranch (b.s.context, b.first, b.s.save));
    CHECK (b.current() == "Idea 2");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK (phazite::getLastVersion (b.s.context)->id == b.first);

    // Main kept its change since its last version.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("A branch that changed other tracks merges with no clashes, and the version after follows both lines")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    b.s.setVolume (b.drums, -9.0f);
    const auto drumsVersion = keepVersion (b.s.context, "Drums down");

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
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
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    b.s.setVolume (b.bass, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Much quieter");

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
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

TEST_CASE ("No switching branches while the song records, which would lose the take, nor a branch from a version")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save)); // from Now: no switch
    auto& song = *phazite::findSong (b.s.context);
    phazite::getSession (b.s.context)->setLiveTrack (b.bass); // live MIDI plays it: it can record

    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    CHECK_FALSE (phazite::canSwitchBranch (b.s.context));
    CHECK_FALSE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK (phazite::findSong (b.s.context) == &song); // the same song, still recording
    CHECK (phazite::isRecording (song));
    CHECK (b.current() == "Idea 2");

    // Nor a branch from an old version, which switches to it; from Now still, which doesn't.
    phazite::selectVersion (b.s.context, b.first);
    CHECK_FALSE (phazite::canMakeBranch (b.s.context));
    CHECK_FALSE (phazite::newBranch (b.s.context, b.first, b.s.save));
    CHECK (phazite::getBranches (b.s.context).size() == 2);

    phazite::stopRecording (song);
    CHECK (phazite::canSwitchBranch (b.s.context));
    CHECK (phazite::canMakeBranch (b.s.context));
}

TEST_CASE ("The branch commands act on the version and the branch the Versions view picks")
{
    Branches b;
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, b.s.context, b.s.save, b.s.renders);

    phazite::selectVersion (b.s.context, {}); // Now
    REQUIRE (commands.invoke ("version.newBranch"));
    CHECK (b.current() == "Idea 2");

    phazite::selectBranch (b.s.context, findBranch (b.s.context, "Main"));
    REQUIRE (commands.invoke ("version.switchBranch"));
    CHECK (b.current() == "Main");
    CHECK (phazite::getCurrentName (*commands.find ("version.merge")) == "Merge into Main...");
    CHECK_FALSE (commands.invoke ("version.finishMerge")); // no merge's list shows
}

TEST_CASE ("A switch makes the song the other branch's in place: its plug-ins both branches have go on, and so does playback")
{
    TestPlugin testPlugin; // known while the song is
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    const auto effect = TestPlugin::add (song, b.bass);
    keepVersion (b.s.context, "With the effect");
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    b.s.setVolume (b.drums, -9.0f);
    b.s.addTrack ("Keys"); // on Idea 2 alone

    const auto instance = phazite::getSession (b.s.context)->getPlugins().find (effect);
    REQUIRE (instance != nullptr);
    EnginePlayback playback;
    phazite::playOrStop (song);
    playback.play (0.5);
    const auto before = phazite::getPlayheadBeat (song);

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK (phazite::findSong (b.s.context) == &song); // the same song, now Main's
    CHECK (b.volume (b.drums) == doctest::Approx (0.0f));
    CHECK (phazite::getTrackIDs (song).size() == 3);                                   // the first track, Bass and Drums
    CHECK (phazite::getSession (b.s.context)->getPlugins().find (effect) == instance); // not loaded again
    CHECK (phazite::isSongPlaying (song));
    playback.play (0.5);
    CHECK (phazite::getPlayheadBeat (song) > before);
    phazite::stopPlaying (song);
}

TEST_CASE ("Each branch keeps its own view state, and a merge waiting for its version stays with the branch it was made on")
{
    Branches b;
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    phazite::setCollapsed (b.s.context, b.drums, true);
    b.s.setVolume (b.bass, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Much quieter");

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK_FALSE (phazite::isCollapsed (*phazite::findSong (b.s.context), b.drums));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter");
    REQUIRE (phazite::startMerge (b.s.context, theirs));
    phazite::finishTickedMerge (b.s.context);
    REQUIRE (phazite::getClashes (b.s.context).size() == 1);

    // On Idea 2, its own view state, and no merge.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Idea 2"), b.s.save));
    CHECK (phazite::isCollapsed (*phazite::findSong (b.s.context), b.drums));
    CHECK_FALSE (phazite::isMerging (b.s.context));
    CHECK (phazite::getClashes (b.s.context).empty());

    // Back on Main, the merge as it was, its clash to switch.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK (phazite::isMerging (b.s.context));
    REQUIRE (phazite::getClashes (b.s.context).size() == 1);
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    REQUIRE (phazite::chooseClash (b.s.context, 0, 2));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("A frozen track stays frozen through a switch where its file holds what it plays there, and plays live where not")
{
    Branches b;
    TemporaryFolder sounds;
    auto& song = *phazite::findSong (b.s.context);
    const auto clip = b.s.addMidiClip (b.bass, 0.0, 4.0);
    b.s.addNote (clip, 48, 0.0, 1.0);
    const auto freeze = [&]
    {
        phazite::freezeFromFile (song, b.bass, writeSound (sounds.folder.getChildFile ("Rendered.wav"), 1.0));
        phazite::test::dispatchPendingMessages();
        REQUIRE (phazite::isFrozen (b.s.context, b.bass));
    };

    freeze();
    keepVersion (b.s.context, "Frozen bass");

    // Idea 2 plays the bass live, with a note more; Main's bass is what its file holds still.
    REQUIRE (phazite::newBranch (b.s.context, {}, b.s.save));
    b.s.addNote (clip, 50, 1.0, 1.0);
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (phazite::isFrozen (b.s.context, b.bass));
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK (phazite::isFrozen (b.s.context, b.bass));

    // Frozen on Idea 2 with that note, its file holds that: on Main, the bass plays live.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Idea 2"), b.s.save));
    CHECK_FALSE (phazite::isFrozen (b.s.context, b.bass)); // as it was left there
    freeze();
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Main"), b.s.save));
    CHECK_FALSE (phazite::isFrozen (b.s.context, b.bass));

    // And on Idea 2 again, frozen, as its file holds.
    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "Idea 2"), b.s.save));
    CHECK (phazite::isFrozen (b.s.context, b.bass));
}
