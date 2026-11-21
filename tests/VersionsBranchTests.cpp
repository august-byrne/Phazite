// SPDX-License-Identifier: AGPL-3.0-or-later
// Branches and merges of a project's versions (docs/design/versions.md: Branches, Merging; step V4): through the model, as the
// Versions view and its commands make them, each switch making the song the other branch's in place.
#include "EnginePlayback.h"
#include "SongIDs.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

namespace
{
using namespace phazite::test;

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

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

// A song with Bass and Drums, and its first version on main.
struct Branches
{
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass"), drums = s.addTrack ("Drums");
    juce::String first = keepVersion (s.context, "First");

    float volume (phazite::TrackID track) { return phazite::getChannelVolume (*phazite::findSong (s.context), track); }
    juce::String current() { return phazite::getCurrentBranch (s.context)->name; }
};
} // namespace

TEST_CASE ("New Branch from Now goes on with the song and its changes on a branch named idea-2, in a color of its own")
{
    Branches b;
    b.s.setVolume (b.bass, -6.0f);
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));

    CHECK (b.current() == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (b.s.context))); // still since the version it was made from
    const auto branches = phazite::getBranches (b.s.context);
    REQUIRE (branches.size() == 2);
    CHECK (branches[0].name == "main");
    CHECK (branches[1].head == b.first);
    CHECK (branches[0].colour != branches[1].colour);

    // The next one is idea-3.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    CHECK (b.current() == "idea-3");
}

TEST_CASE ("A branch named with a space keeps its name as typed, through the store and as the project opens again")
{
    Branches b;
    const auto made = phazite::newBranchNamed (b.s.context, "  Half   time chorus ");
    REQUIRE (made.isNotEmpty());
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "Half time chorus");
    CHECK (findBranch (b.s.context, "Half time chorus") == made);

    // Renamed, spaces and all; another branch's name, in any case, is refused.
    REQUIRE (phazite::renameBranch (b.s.context, made, "Half time, take 2"));
    CHECK_FALSE (phazite::renameBranch (b.s.context, made, "MAIN"));
    CHECK (b.current() == "Half time, take 2");

    b.s.reopen();
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "Half time, take 2");
    CHECK (phazite::getCurrentBranch (b.s.context)->id == made);
}

TEST_CASE ("Switching keeps each branch's song as it was left there, unsaved changes included, and starts the undo history again")
{
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter bass");
    b.s.setVolume (b.bass, -9.0f); // not in a version

    // main as its last version had it.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK (b.current() == "main");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK_FALSE (phazite::canUndo (*phazite::findSong (b.s.context)));
    CHECK_FALSE (b.s.getProject().hasUnsavedChanges()); // the project file is main's song now

    b.s.addTrack ("Keys"); // left on main

    // idea-2 as it was left, its change since its version too.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK (b.current() == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f));
    CHECK (phazite::getChangesSinceLastVersion (withChangesIn (b.s.context)).size() == 1);
    CHECK (phazite::getTrackIDs (*phazite::findSong (b.s.context)).size() == 3);

    // And main with Keys.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK (phazite::getTrackIDs (*phazite::findSong (b.s.context)).size() == 4);

    // It opens again on the branch it was on.
    b.s.reopen();
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "main");
}

TEST_CASE ("New Branch from a version opens that version's song on the new branch")
{
    Branches b;
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Second");
    b.s.setVolume (b.bass, -12.0f);

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, b.first, b.s.save); }));
    CHECK (b.current() == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK (phazite::getLastVersion (b.s.context)->id == b.first);

    // main kept its change since its last version.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("A branch that changed other tracks merges with no clashes, and the version after follows both lines")
{
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.drums, -9.0f);
    const auto drumsVersion = keepVersion (b.s.context, "Drums down");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    b.s.setVolume (b.bass, -6.0f);
    const auto bassVersion = keepVersion (b.s.context, "Bass down");

    // The version the current branch has can't be merged; the other's can.
    phazite::selectVersion (b.s.context, b.first);
    CHECK_FALSE (phazite::canMerge (b.s.context));
    phazite::selectVersion (b.s.context, drumsVersion);
    REQUIRE (phazite::canMerge (b.s.context));

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, drumsVersion); }));
    CHECK (phazite::describeMerge (b.s.context) == "Drums down");
    const auto lines = phazite::getMergeLines (b.s.context);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0].change.name == "Drums");
    CHECK_FALSE (lines[0].clashes);

    phazite::finishMerge (b.s.context);
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    CHECK (b.volume (b.drums) == doctest::Approx (-9.0f));
    CHECK (phazite::describeUndo (phazite::findSong (b.s.context)) == "Undo Merge idea-2");
    CHECK (phazite::isMerging (b.s.context));
    CHECK (phazite::getClashes (b.s.context).empty());

    // The version that finishes it follows both, and is named for what it merged.
    const auto merged = keepVersion (b.s.context);
    CHECK_FALSE (phazite::isMerging (b.s.context));
    const auto version = phazite::getLastVersion (b.s.context);
    CHECK (version->parents == juce::StringArray (bassVersion, drumsVersion));
    CHECK (version->name == "Merge idea-2");
    juce::ignoreUnused (merged);

    // Merged once, the other's version is in the current branch's past.
    phazite::selectVersion (b.s.context, drumsVersion);
    CHECK_FALSE (phazite::canMerge (b.s.context));
}

TEST_CASE ("A knob both branches changed is a clash, which switches between its three values, each an undo step")
{
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Much quieter");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter");

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    const auto lines = phazite::getMergeLines (b.s.context);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0].clashes);

    phazite::finishMerge (b.s.context);

    const auto clashes = phazite::getClashes (b.s.context);
    REQUIRE (clashes.size() == 1);
    CHECK (clashes[0].line == "Bass");
    CHECK (clashes[0].what == "volume");
    CHECK (clashes[0].chosen == 1);
    CHECK (clashes[0].otherBranch == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f)); // this branch's, to start with

    REQUIRE (phazite::chooseClash (b.s.context, 0, 2));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
    CHECK (phazite::describeUndo (phazite::findSong (b.s.context)) == "Undo Take the Other Branch's: Bass volume");
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 2);

    REQUIRE (phazite::chooseClash (b.s.context, 0, 0));
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f)); // as before either changed it

    phazite::undo (*phazite::findSong (b.s.context));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
}

TEST_CASE ("No switching branches while the song records, which would lose the take, nor a branch from a version")
{
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); })); // from Now: no switch
    auto& song = *phazite::findSong (b.s.context);
    phazite::getSession (b.s.context)->setLiveTrack (b.bass); // live MIDI plays it: it can record

    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    CHECK_FALSE (phazite::canSwitchBranch (b.s.context));
    CHECK_FALSE (phazite::test::finished (b.s.context,
                                          [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK (phazite::findSong (b.s.context) == &song); // the same song, still recording
    CHECK (phazite::isRecording (song));
    CHECK (b.current() == "idea-2");

    // Nor a branch from an old version, which switches to it; from Now still, which doesn't.
    phazite::selectVersion (b.s.context, b.first);
    CHECK_FALSE (phazite::canMakeBranch (b.s.context));
    CHECK_FALSE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, b.first, b.s.save); }));
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
    REQUIRE (phazite::test::finished (b.s.context, [&] { return commands.invoke ("version.newBranch"); }));
    CHECK (b.current() == "idea-2");

    phazite::selectBranch (b.s.context, findBranch (b.s.context, "main"));
    REQUIRE (phazite::test::finished (b.s.context, [&] { return commands.invoke ("version.switchBranch"); }));
    CHECK (b.current() == "main");
    CHECK (phazite::getCurrentName (*commands.find ("version.merge")) == "Merge into 'main'...");
    CHECK_FALSE (commands.invoke ("version.finishMerge")); // no merge's list shows
}

TEST_CASE ("A switch makes the song the other branch's in place: its plug-ins both branches have go on, and so does playback")
{
    TestPlugin testPlugin; // known while the song is
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    const auto effect = TestPlugin::add (song, b.bass);
    keepVersion (b.s.context, "With the effect");
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.drums, -9.0f);
    b.s.addTrack ("Keys"); // on idea-2 alone

    const auto instance = phazite::getSession (b.s.context)->getPlugins().find (effect);
    REQUIRE (instance != nullptr);
    EnginePlayback playback;
    phazite::playOrStop (song);
    playback.play (0.5);
    const auto before = phazite::getPlayheadBeat (song);

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK (phazite::findSong (b.s.context) == &song); // the same song, now main's
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
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    phazite::setCollapsed (b.s.context, b.drums, true);
    b.s.setVolume (b.bass, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Much quieter");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK_FALSE (phazite::isCollapsed (*phazite::findSong (b.s.context), b.drums));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter");
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    REQUIRE (phazite::getClashes (b.s.context).size() == 1);

    // On idea-2, its own view state, and no merge.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK (phazite::isCollapsed (*phazite::findSong (b.s.context), b.drums));
    CHECK_FALSE (phazite::isMerging (b.s.context));
    CHECK (phazite::getClashes (b.s.context).empty());

    // Back on main, the merge as it was, its clash to switch.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
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

    // idea-2 plays the bass live, with a note more; main's bass is what its file holds still.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.addNote (clip, 50, 1.0, 1.0);
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (phazite::isFrozen (b.s.context, b.bass));
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK (phazite::isFrozen (b.s.context, b.bass));

    // Frozen on idea-2 with that note, its file holds that: on main, the bass plays live.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK_FALSE (phazite::isFrozen (b.s.context, b.bass)); // as it was left there
    freeze();
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    CHECK_FALSE (phazite::isFrozen (b.s.context, b.bass));

    // And on idea-2 again, frozen, as its file holds.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK (phazite::isFrozen (b.s.context, b.bass));
}

TEST_CASE ("Delete Branch takes another branch away, with the commits only it had, which stay in the store to recover")
{
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::newBranch (b.s.context, {}, b.s.save); })); // idea-2, nothing of its own yet
    const auto empty = findBranch (b.s.context, "idea-2");
    REQUIRE (phazite::test::finished (
        b.s.context,
        [&] { return phazite::newBranch (b.s.context, {}, b.s.save); })); // idea-3, with a commit of its own and a change left on it
    const auto idea = findBranch (b.s.context, "idea-3");
    b.s.setVolume (b.drums, -9.0f);
    keepVersion (b.s.context, "Quiet drums");
    b.s.setVolume (b.drums, -12.0f);
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));

    // Not the branch the song is on; what goes with each, said before it goes.
    CHECK_FALSE (phazite::canDeleteBranch (b.s.context, findBranch (b.s.context, "main")));
    CHECK (phazite::canDeleteBranch (b.s.context, idea));
    CHECK (phazite::canDeleteAnyBranch (b.s.context));
    CHECK (phazite::describeBranchDeletion (b.s.context, empty).isEmpty());

    // Whether the song left there changed is found out on the history's thread as the picker lists the branch (canDeleteBranch()
    // asks; known bug 126): said as maybe until it is known.
    CHECK (phazite::describeBranchDeletion (b.s.context, idea).contains ("if it has any"));
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (phazite::describeBranchDeletion (b.s.context, idea)
           == "This also deletes 1 version that no other branch has, and the changes left on it. Recover Lost Versions brings "
              "them back for 30 days.");

    // Gone, its commit out of the graph, and so on every computer as the project opens again.
    REQUIRE (phazite::deleteBranch (b.s.context, idea));
    REQUIRE (phazite::deleteBranch (b.s.context, empty));
    CHECK (phazite::getBranches (b.s.context).size() == 1);
    CHECK (phazite::getVersions (b.s.context).size() == 2); // the initial commit and First
    CHECK_FALSE (phazite::canDeleteAnyBranch (b.s.context));
    b.s.reopen();
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (phazite::getBranches (b.s.context).size() == 1);
    CHECK (b.current() == "main");

    // Its version and the song left on it are still in the store: Recover Lost Versions lists the deleted branch, with when
    // it was deleted, and brings it back with the song as it was left there. The empty one left nothing to bring back.
    const auto lost = phazite::getLostVersions (b.s.context);
    REQUIRE (lost.size() == 1);
    CHECK (lost[0].why == "delete");
    CHECK (lost[0].branch == "idea-3");
    CHECK (lost[0].numVersions == 1);
    CHECK (lost[0].withSong);
    CHECK (lost[0].lastName == "Quiet drums");
    const auto recovered = phazite::recoverLostVersions (b.s.context, lost[0].key);
    REQUIRE (recovered.isNotEmpty());
    CHECK (phazite::getBranches (b.s.context).size() == 2);
    CHECK (findBranch (b.s.context, "idea-3 (recovered)") == recovered);
    CHECK (phazite::getLostVersions (b.s.context).empty());

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::switchBranch (b.s.context, recovered, b.s.save); }));
    CHECK (phazite::getChannelVolume (*phazite::findSong (b.s.context), b.drums) == doctest::Approx (-12.0f));
    const auto changes = phazite::getChangesSinceLastVersion (withChangesIn (b.s.context));
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].what == "volume");
}

TEST_CASE ("A branch deleted on one computer comes back where another computer committed on it since")
{
    // Every computer's record of one branch, as the Versions folder holds them, and the commits they name.
    std::map<juce::String, phazite::VersionRecord> versions;
    versions["a"].id = "a";
    versions["b"].id = "b";
    versions["b"].parents.add ("a");

    const auto record = [] (const juce::String& computer, const juce::String& head, juce::int64 changed, bool deleted)
    {
        phazite::BranchRecord branch;
        branch.id = "idea";
        branch.name = "idea-2";
        branch.computer = computer;
        branch.head = head;
        branch.changed = changed;
        branch.deleted = deleted;
        return branch;
    };

    // Deleted here at its last commit, the newest word: gone.
    CHECK (phazite::joinBranchRecords ({ record ("here", "b", 2000, true), record ("there", "a", 1000, false) }, versions, "here").empty());

    // Committed on elsewhere past where it was deleted: back, at that commit.
    const auto back =
        phazite::joinBranchRecords ({ record ("here", "a", 2000, true), record ("there", "b", 1000, false) }, versions, "here");
    REQUIRE (back.size() == 1);
    CHECK (back.front().head == "b");
    CHECK_FALSE (back.front().deleted);

    // Named again elsewhere since: the newer word.
    CHECK (phazite::joinBranchRecords ({ record ("here", "b", 2000, true), record ("there", "b", 3000, false) }, versions, "here").size()
           == 1);
}

TEST_CASE ("Merge Branch merges another branch's last commit, and Cancel Merge makes the song what it was before it")
{
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    const auto idea = findBranch (b.s.context, "idea-2");
    b.s.setVolume (b.drums, -9.0f);
    keepVersion (b.s.context, "Quiet drums");
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    const auto main = findBranch (b.s.context, "main");

    // Another branch with commits main doesn't have; not main itself.
    CHECK (phazite::countCommitsToMerge (b.s.context, idea) == 1);
    CHECK (phazite::canMergeBranch (b.s.context, idea));
    CHECK_FALSE (phazite::canMergeBranch (b.s.context, main));
    CHECK (phazite::canMergeAnyBranch (b.s.context));

    // Its lines listed, taken, then aborted: the song as it was, one undo step, and no merge waiting for a commit.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::mergeBranch (b.s.context, idea); }));
    REQUIRE_FALSE (phazite::getMergeLines (b.s.context).empty());
    phazite::finishMerge (b.s.context);
    CHECK (phazite::isMerging (b.s.context));
    CHECK (b.volume (b.drums) == doctest::Approx (-9.0f));
    CHECK (phazite::canCancelMerge (b.s.context));
    phazite::cancelMerge (b.s.context);
    CHECK_FALSE (phazite::isMerging (b.s.context));
    CHECK (b.volume (b.drums) == doctest::Approx (0.0f));
    CHECK (phazite::describeUndo (phazite::findSong (b.s.context)) == "Undo Cancel Merge");

    // Merged and committed: nothing more to merge from it.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::mergeBranch (b.s.context, idea); }));
    phazite::finishMerge (b.s.context);
    keepVersion (b.s.context);
    CHECK (phazite::getLastVersion (b.s.context)->name == "Merge idea-2");
    CHECK (phazite::countCommitsToMerge (b.s.context, idea) == 0);
    CHECK_FALSE (phazite::canMergeBranch (b.s.context, idea));
    CHECK_FALSE (phazite::canMergeAnyBranch (b.s.context));
}

TEST_CASE ("A branch that ungrouped a folder and moved a clip merges with the folder's tracks kept and the clip once, as it changed here")
{
    // main: Bass and Drums in a folder, a clip on Bass.
    Branches b;
    auto& song = *phazite::findSong (b.s.context); // the same through each switch, which is made in place
    const auto clip = b.s.addMidiClip (b.bass, 0.0, 4.0);
    b.s.addNote (clip, 36, 0.0, 1.0);
    phazite::TrackID folder;

    {
        const phazite::UndoStep step (song, "Group");
        folder = phazite::groupIntoFolder (song, { b.bass, b.drums }, nullptr);
    }

    keepVersion (b.s.context, "Grouped");

    // idea-2: the folder ungrouped, and the clip onto Drums.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));

    {
        const phazite::UndoStep step (song, "Ungroup");
        phazite::ungroupFolder (song, folder);
        phazite::moveClip (song, clip, phazite::BeatPosition::fromBeats (4.0), b.drums);
    }

    const auto theirs = keepVersion (b.s.context, "Ungrouped");

    // main: a note in the clip.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    REQUIRE (phazite::getTracksInFolder (song, folder) == std::vector { b.bass, b.drums });
    b.s.addNote (clip, 40, 2.0, 1.0);
    keepVersion (b.s.context, "Note");
    const auto before = phazite::test::copyWholeSong (song);

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    REQUIRE (phazite::isMerging (b.s.context));
    checkIDs (song);
    CHECK_FALSE (phazite::findTrackState (song, folder).isValid());
    CHECK (phazite::findTrackState (song, b.bass).isValid());
    CHECK (phazite::findTrackState (song, b.drums).isValid());
    CHECK (phazite::getParentFolder (song, b.bass) == phazite::TrackID());
    CHECK (phazite::getClips (song, b.bass).empty());
    REQUIRE (phazite::getClips (song, b.drums) == std::vector { clip });
    CHECK (phazite::getNotes (b.s.context, { clip }).size() == 2);
    CHECK (phazite::getClashes (b.s.context).empty());

    // One undo step: the song as it was before the merge; and redone, as merged.
    phazite::undo (song);
    checkIDs (song);
    CHECK (phazite::findChanges (before, song.getState()).empty());
    phazite::redo (song);
    checkIDs (song);
    CHECK (phazite::getClips (song, b.drums) == std::vector { clip });

    // Aborted: as before it too; then merged again.
    phazite::cancelMerge (b.s.context);
    checkIDs (song);
    CHECK (phazite::findChanges (before, song.getState()).empty());
    CHECK (phazite::getTracksInFolder (song, folder) == std::vector { b.bass, b.drums });
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    checkIDs (song);
    CHECK_FALSE (phazite::findTrackState (song, folder).isValid());
    CHECK (phazite::getClips (song, b.drums) == std::vector { clip });

    // Committed, and opened again: each ID once.
    keepVersion (b.s.context);
    b.s.reopen();
    REQUIRE (waitForVersionHistory (b.s.context));
    checkIDs (*phazite::findSong (b.s.context));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (b.s.context)));
}

TEST_CASE ("A merge leaves out a send the other side added to a return this side deleted, with what modulated it")
{
    // Deleting the return took its sends away here; the send the other side added came in with the merge, naming a return
    // the song hadn't, and Phazite saved what the next opening changed.
    Branches b;
    const auto reverb = b.s.addReturn (b.bass, "phaziteReverb");
    keepVersion (b.s.context, "Reverb");

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.addSend (b.drums, reverb, -12.0f);
    const auto send = phazite::getSends (*phazite::findSong (b.s.context), b.drums).front();
    const auto lfo = b.s.addModulator (b.drums, phazite::ModulatorType::lfo);
    b.s.modulate (lfo, { send, phazite::schema::sendLevelParameter }, 0.5f);
    const auto drumsSend = keepVersion (b.s.context, "Drums to the reverb");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));

    {
        auto& mainSong = *phazite::findSong (b.s.context);
        const phazite::UndoStep step (mainSong, "Delete Track");
        phazite::deleteTracks (mainSong, { reverb }, nullptr);
    }

    keepVersion (b.s.context, "No reverb");
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, drumsSend); }));
    phazite::finishMerge (b.s.context);

    auto& merged = *phazite::findSong (b.s.context);
    CHECK_FALSE (phazite::findTrackState (merged, reverb).isValid());
    CHECK (phazite::getSends (merged, b.drums).empty());
    checkIDs (merged.getState());
}

TEST_CASE ("A merge lets go of a sidechain that the two lines' changes made into a loop, as the edit that makes one does")
{
    // main: Bass's compressor listens to the reverb; idea-2: Bass sends to the reverb. Each alone is no loop; merged, the
    // compressor would hear its own track's sound, which Phazite's engine can't play, while its picker named the reverb.
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    const auto reverb = b.s.addReturn (b.drums, "phaziteReverb");
    const auto compressor = b.s.addEffect (b.bass, phazite::devices::Compressor::typeID);
    keepVersion (b.s.context, "Reverb");

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.addSend (b.bass, reverb, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Bass to the reverb");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    phazite::setSidechainSource (song, compressor, reverb);
    REQUIRE (phazite::getSidechainSource (song, compressor) == reverb);
    keepVersion (b.s.context, "Ducked under the reverb");

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    UndoCheck undoCheck (b.s.context);
    undoCheck.check ("Merge", [&b] { phazite::finishMerge (b.s.context); });
    REQUIRE (phazite::isMerging (b.s.context));
    CHECK (phazite::getSends (song, b.bass).size() == 1);
    CHECK_FALSE (phazite::getSidechainSource (song, compressor).isValid());
    checkIDs (song);

    // One undo step with the merge: undone, the sidechain is back, as it was.
    phazite::undo (song);
    CHECK (phazite::getSidechainSource (song, compressor) == reverb);
    CHECK (phazite::getSends (song, b.bass).empty());
}

TEST_CASE ("A merge's conflict answered for the other side leaves out what that side's value names and this side deleted")
{
    // Both lines pointed Bass's compressor at another track; main deleted the one idea-2 chose. Take the Other Branch's
    // names a track the song hasn't, which the next opening would leave out: it goes, in the choice's step.
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    const auto kick = b.s.addTrack ("Kick"), snare = b.s.addTrack ("Snare");
    const auto compressor = b.s.addEffect (b.bass, phazite::devices::Compressor::typeID);
    keepVersion (b.s.context, "Drums");

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    phazite::setSidechainSource (song, compressor, snare);
    REQUIRE (phazite::getSidechainSource (song, compressor) == snare);
    const auto theirs = keepVersion (b.s.context, "Under the snare");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    phazite::setSidechainSource (song, compressor, kick);

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { snare }, nullptr);
    }

    keepVersion (b.s.context, "Under the kick, no snare");
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    CHECK (phazite::getSidechainSource (song, compressor) == kick); // this side's, to start with

    const auto clashes = phazite::getClashes (b.s.context);
    REQUIRE (clashes.size() == 1);
    UndoCheck undoCheck (b.s.context);
    undoCheck.check ("Take the Other Branch's", [&b] { REQUIRE (phazite::chooseClash (b.s.context, 0, 2)); });
    CHECK_FALSE (phazite::findTrackState (song, snare).isValid());
    CHECK_FALSE (song.getState().toXmlString().contains ("sidechain=\"" + juce::String ((juce::uint64) snare.getRaw()) + "\""));
    CHECK_FALSE (phazite::getSidechainSource (song, compressor).isValid());
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 2);

    // Undone: the kick again, and the conflict as it was.
    phazite::undo (song);
    CHECK (phazite::getSidechainSource (song, compressor) == kick);
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 1);
}

TEST_CASE ("Undo after a merge takes the merge back with the song, Redo brings it back, and a merge waits still as the project opens again")
{
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -12.0f);
    const auto theirs = keepVersion (b.s.context, "Much quieter");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    b.s.setVolume (b.bass, -6.0f);
    const auto quieter = keepVersion (b.s.context, "Quieter");

    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    REQUIRE (phazite::isMerging (b.s.context));
    REQUIRE (phazite::getClashes (b.s.context).size() == 1);

    // Undone: no merge waits, as git has none after a reset; redone, it waits again, as it was.
    phazite::undo (song);
    CHECK_FALSE (phazite::isMerging (b.s.context));
    CHECK_FALSE (phazite::hasSomethingToCommit (withChangesIn (b.s.context)));
    phazite::redo (song);
    CHECK (phazite::isMerging (b.s.context));
    REQUIRE (phazite::getClashes (b.s.context).size() == 1);

    // A conflict's choice, undone with its value.
    REQUIRE (phazite::chooseClash (b.s.context, 0, 2));
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
    phazite::undo (song);
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 1);
    phazite::redo (song);
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 2);

    // Cancel Merge, undone: the merge waits again, its choice as it was.
    phazite::cancelMerge (b.s.context);
    CHECK_FALSE (phazite::isMerging (b.s.context));
    CHECK (b.volume (b.bass) == doctest::Approx (-6.0f));
    phazite::undo (song);
    REQUIRE (phazite::isMerging (b.s.context));
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 2);
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));

    // Closed and opened again: it waits still, its conflict as chosen, to switch as before.
    REQUIRE (waitForVersionHistory (b.s.context));
    b.s.reopen();
    REQUIRE (waitForVersionHistory (b.s.context));
    auto& reopened = *phazite::findSong (b.s.context);
    REQUIRE (phazite::isMerging (b.s.context));
    CHECK (phazite::describeMerge (b.s.context) == "Much quieter");
    REQUIRE (phazite::getClashes (b.s.context).size() == 1);
    CHECK (phazite::getClashes (b.s.context)[0].chosen == 2);
    CHECK (phazite::getClashes (b.s.context)[0].otherBranch == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-12.0f));
    REQUIRE (phazite::chooseClash (b.s.context, 0, 0));
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));

    // Its commit follows both lines; then nothing waits, here or as the project opens again.
    keepVersion (b.s.context);
    CHECK (phazite::getLastVersion (b.s.context)->parents == juce::StringArray (quieter, theirs));
    CHECK_FALSE (phazite::isMerging (b.s.context));
    b.s.reopen();
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK_FALSE (phazite::isMerging (b.s.context));
    juce::ignoreUnused (reopened);
}

TEST_CASE ("A merge undone leaves the next commit on this line alone, and an undo after the merge's commit changes the song alone")
{
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.drums, -9.0f);
    const auto theirs = keepVersion (b.s.context, "Quiet drums");
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));

    // Merged, undone, and something else committed: one parent.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    phazite::undo (song);
    b.s.setVolume (b.bass, -3.0f);
    keepVersion (b.s.context, "Bass a little down");
    CHECK (phazite::getLastVersion (b.s.context)->parents.size() == 1);

    // Merged and committed; then undone and redone: no merge waits again.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, theirs); }));
    phazite::finishMerge (b.s.context);
    keepVersion (b.s.context);
    CHECK (phazite::getLastVersion (b.s.context)->parents.size() == 2);
    phazite::undo (song);
    CHECK_FALSE (phazite::isMerging (b.s.context));
    phazite::redo (song);
    CHECK_FALSE (phazite::isMerging (b.s.context));
}

TEST_CASE ("A line merged again after it made an item with an ID a merge gave to another of its own merges each onto its own, each ID once")
{
    // Two computers on one history, played on one: the other line's count stays its own, as its computer never saw this
    // line's (a switch here would count both on past either).
    Branches b;
    auto& song = *phazite::findSong (b.s.context);
    const auto count = song.getState()[phazite::schema::nextID];
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    const auto pad = b.s.addTrack ("Pad");
    const auto t1 = keepVersion (b.s.context, "Pad");

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    song.getState().setProperty (phazite::schema::nextID, count, nullptr);
    const auto lead = b.s.addTrack ("Lead");
    REQUIRE (lead == pad); // counted on from the same place
    keepVersion (b.s.context, "Lead");

    const auto findNamed = [&song] (const juce::String& name)
    {
        std::vector<phazite::TrackID> found;

        for (const auto track : phazite::getTrackIDs (song))
            if (phazite::getTrackName (song, track) == name)
                found.push_back (track);

        return found;
    };

    // The first merge gives idea-2's Pad a new ID here.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, t1); }));
    phazite::finishMerge (b.s.context);
    keepVersion (b.s.context);
    REQUIRE (findNamed ("Pad").size() == 1);
    const auto padHere = findNamed ("Pad").front();
    REQUIRE (padHere != pad);
    const auto numTracks = phazite::getTrackIDs (song).size();

    // idea-2, as the other computer has it: its next track gets the ID the merge gave its Pad here; and its Pad renamed.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    song.getState().setProperty (phazite::schema::nextID, (juce::int64) padHere.getRaw(), nullptr);
    const auto strings = b.s.addTrack ("Strings");
    REQUIRE (strings == padHere);
    b.s.rename (pad, "Pad 2");
    const auto t2 = keepVersion (b.s.context, "Strings");

    // Merged again: Pad 2 onto the Pad here, Strings beside it with an ID of its own.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, t2); }));
    phazite::finishMerge (b.s.context);
    checkIDs (song);
    CHECK (phazite::getClashes (b.s.context).empty());
    CHECK (findNamed ("Pad").empty());
    CHECK (findNamed ("Pad 2") == std::vector { padHere });
    CHECK (findNamed ("Lead") == std::vector { lead });
    REQUIRE (findNamed ("Strings").size() == 1);
    const auto stringsHere = findNamed ("Strings").front();
    CHECK (stringsHere != padHere);
    CHECK (phazite::getTrackIDs (song).size() == numTracks + 1);
    keepVersion (b.s.context);

    // And once more, through both merges' records: idea-2's Strings renamed lands on the Strings here.
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    b.s.rename (strings, "Strings 2");
    const auto t3 = keepVersion (b.s.context, "Strings 2");
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::startMerge (b.s.context, t3); }));
    phazite::finishMerge (b.s.context);
    checkIDs (song);
    CHECK (findNamed ("Strings").empty());
    CHECK (findNamed ("Strings 2") == std::vector { stringsHere });
    CHECK (findNamed ("Pad 2") == std::vector { padHere });
    CHECK (phazite::getTrackIDs (song).size() == numTracks + 1);
}

TEST_CASE ("A switch reads on the history's thread, the window held meanwhile: nothing that changes the song runs, and Cancel leaves it")
{
    // Known bug 126: a switch read the branch's song and its last commit's on the message thread. Now it holds the window
    // (ProjectContext::holdForWork()) from its start until what was read is taken in; a command that changes the song isn't
    // run meanwhile; and the progress window's Cancel leaves the song and the branch as they were.
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter bass");
    b.s.setVolume (b.bass, -9.0f); // left on idea-2

    std::vector<juce::String> holds;
    std::function<void (const juce::String&)> cancel;
    struct Hold final : phazite::ProjectContext::WorkHold
    {
        std::vector<juce::String>& holds;
        explicit Hold (std::vector<juce::String>& h)
            : holds (h)
        {
        }
        ~Hold() override { holds.push_back ("released"); }
    };

    b.s.context.setWorkHolder (
        [&] (const juce::String& title, std::function<void (const juce::String&)> cancelIt, phazite::ProjectContext::Progress)
        {
            holds.push_back (title);
            cancel = std::move (cancelIt);
            return std::make_unique<Hold> (holds);
        });

    const auto main = findBranch (b.s.context, "main");
    const auto tracks = phazite::getTrackIDs (*phazite::findSong (b.s.context)).size();
    REQUIRE (phazite::switchBranch (b.s.context, main, b.s.save));
    REQUIRE (holds.size() == 1);
    CHECK (holds[0] == "Reading branch 'main'");
    CHECK (b.s.context.isHoldingForWork());
    CHECK_FALSE (phazite::canSwitchBranch (b.s.context)); // one at a time
    CHECK_FALSE (b.s.commands.invoke ("track.add"));      // nothing that changes the song runs in between
    CHECK (phazite::getTrackIDs (*phazite::findSong (b.s.context)).size() == tracks);

    // Cancelled before what it read is in: nothing changes, then or after.
    cancel ({});
    CHECK (holds.back() == "released");
    CHECK_FALSE (b.s.context.isHoldingForWork());
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f));
    CHECK (phazite::getChangesSinceLastVersion (withChangesIn (b.s.context)).size() == 1);

    // Again, to its end: the hold goes once the song is main's.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::switchBranch (b.s.context, main, b.s.save); }));
    CHECK (holds.back() == "released");
    CHECK (b.current() == "main");
    CHECK (b.volume (b.bass) == doctest::Approx (0.0f));
    CHECK (phazite::test::finished (b.s.context,
                                    [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f)); // the change left there kept, as it was copied
}

TEST_CASE ("A switch keeps on the branch it leaves what a plug-in changed in the song while the other was read")
{
    // The song left is copied as the switch starts, and split on the history's thread; changed meanwhile (a plug-in's own
    // change, which comes in while the window is held), it is split again as it is when the other is taken in.
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -2.0f); // a version records a change (one of none is refused)
    keepVersion (b.s.context, "Bass down");
    const auto main = findBranch (b.s.context, "main");
    REQUIRE (phazite::switchBranch (b.s.context, main, b.s.save));
    b.s.setVolume (b.drums, -4.0f); // as a change that comes in while it reads
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getVersionHistory (b.s.context)->isReadingForCommand(); },
                                                   versionHistoryTimeout));
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "main");
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK (b.volume (b.drums) == doctest::Approx (-4.0f));
}

TEST_CASE ("Create Branch from a commit leaves no branch behind when its switch is cancelled or can't be made")
{
    // Found in review (known bug 126's round): the branch was made and written before the switch read, and stayed when the
    // switch was cancelled.
    Branches b;
    b.s.setVolume (b.bass, -2.0f); // a version records a change (one of none is refused)
    keepVersion (b.s.context, "Second");
    const auto before = phazite::getBranches (b.s.context).size();

    REQUIRE (phazite::newBranch (b.s.context, b.first, b.s.save)); // reading
    phazite::getVersionHistory (b.s.context)->cancelReading();     // the progress window's Cancel
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (phazite::getBranches (b.s.context).size() == before);
    CHECK (b.current() == "main");

    // Done to its end, it is there, and the song on it.
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, b.first, b.s.save); }));
    CHECK (phazite::getBranches (b.s.context).size() == before + 1);
    CHECK (b.current() == "idea-2");
}

TEST_CASE ("A switch or a merge that can't be taken in says why, where the Versions view says its problems")
{
    // Found in review (known bug 126's round): it ended silently after its progress window had shown.
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -6.0f);
    keepVersion (b.s.context, "Quieter");
    auto& song = *phazite::findSong (b.s.context);
    const auto main = findBranch (b.s.context, "main");

    SUBCASE ("Recording started while it read")
    {
        phazite::getSession (b.s.context)->setLiveTrack (b.bass);
        REQUIRE (phazite::switchBranch (b.s.context, main, b.s.save));
        phazite::startRecording (song); // a key that started it, which the hold lets through as play does
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getVersionHistory (b.s.context)->isReadingForCommand(); },
                                                       versionHistoryTimeout));
        CHECK (b.current() == "idea-2");
        CHECK (phazite::getReadingProblem (b.s.context).contains ("recording"));
        phazite::stopRecording (song);
    }

    SUBCASE ("The song left on the branch can't be read")
    {
        // main's song as it was left there, its piece gone from the store (as a sync that hasn't finished leaves it).
        REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::switchBranch (b.s.context, main, b.s.save); }));
        b.s.setVolume (b.drums, -3.0f); // left on main
        REQUIRE (phazite::test::finished (b.s.context, [&]
                                          { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
        auto* history = phazite::getVersionHistory (b.s.context);
        phazite::BranchRecord broken;

        for (const auto& branch : history->getBranches())
            if (branch.id == main)
                broken = branch;

        REQUIRE (broken.working.isNotEmpty());
        broken.working = juce::String::repeatedString ("0", 64); // a piece the store hasn't
        REQUIRE (history->getStore()->writeBranch (broken));
        b.s.reopen (false);
        REQUIRE (waitForVersionHistory (b.s.context));

        CHECK_FALSE (phazite::test::finished (b.s.context, [&] { return phazite::switchBranch (b.s.context, main, b.s.save); }));
        CHECK (b.current() == "idea-2");
        CHECK (phazite::getReadingProblem (b.s.context).contains ("can't be read"));
    }
}

TEST_CASE ("A switch made while the song changes splits and keeps the song left on the history's thread, not on the message thread")
{
    // Found in review (known bug 126's round): a change while the other was read (a plug-in's own, the automation written
    // as it plays) sent the split and the write of the whole song left back onto the message thread.
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -2.0f); // a version records a change (one of none is refused)
    keepVersion (b.s.context, "Bass down");
    const auto splits = phazite::countSongsSplit();
    const auto writes = phazite::VersionStore::countSongsWritten();

    REQUIRE (phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save));
    b.s.setVolume (b.drums, -4.0f); // as a change that comes in while it reads
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getVersionHistory (b.s.context)->isReadingForCommand(); },
                                                   versionHistoryTimeout));
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "main");
    CHECK (phazite::countSongsSplit().onMessageThread == splits.onMessageThread);
    CHECK (phazite::VersionStore::countSongsWritten().onMessageThread == writes.onMessageThread);
    CHECK (phazite::countSongsSplit().all > splits.all + 1); // split twice, the second from the song as it changed

    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "idea-2"), b.s.save); }));
    CHECK (b.volume (b.drums) == doctest::Approx (-4.0f));
}

TEST_CASE ("A switch whose records can't all be written leaves every branch's record as it was, the song left on each named still")
{
    // The second review of known bug 126: with the current branch's file failing to be written, the rollback wrote back only
    // the record of the branch left, and the branch entered no longer named the song left there.
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -2.0f); // a version records a change (one of none is refused)
    keepVersion (b.s.context, "Idea");
    b.s.setVolume (b.bass, -9.0f); // left on idea-2
    const auto idea = findBranch (b.s.context, "idea-2");
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));

    {
        // The current branch's file fails, as a full drive makes it.
        const phazite::WholeFileHook failure ([] (const juce::File& file) { return file.getFileName() != "current"; });
        CHECK_FALSE (phazite::test::finished (b.s.context, [&] { return phazite::switchBranch (b.s.context, idea, b.s.save); }));
        CHECK (phazite::getReadingProblem (b.s.context).contains ("couldn't write"));
    }

    CHECK (b.current() == "main");

    // What is on disk names the song left on idea-2 still: opened again, a switch there finds it.
    b.s.reopen (false);
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "main");

    // The entered branch's record isn't written by the switch: it names the song left there until the project file holds the
    // song (VersionHistory::switchSaved()). Failing to be written then, it names it still, which loses nothing.
    {
        const phazite::WholeFileHook failure ([&idea] (const juce::File& file) { return ! file.getFullPathName().contains (idea); });
        REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::switchBranch (b.s.context, idea, b.s.save); }));
    }

    CHECK (b.current() == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f));
    b.s.reopen (false);
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (b.current() == "idea-2");
    CHECK (b.volume (b.bass) == doctest::Approx (-9.0f));
}

TEST_CASE ("Delete Branch's question says when what was left on the branch can't be read, rather than that nothing was")
{
    // The second review of known bug 126: a song left on a branch that couldn't be read counted as no changes, so the branch
    // went without the question.
    Branches b;
    REQUIRE (phazite::test::finished (b.s.context, [&] { return phazite::newBranch (b.s.context, {}, b.s.save); }));
    b.s.setVolume (b.bass, -9.0f); // left on idea-2
    const auto idea = findBranch (b.s.context, "idea-2");
    REQUIRE (phazite::test::finished (b.s.context,
                                      [&] { return phazite::switchBranch (b.s.context, findBranch (b.s.context, "main"), b.s.save); }));

    auto* history = phazite::getVersionHistory (b.s.context);
    phazite::BranchRecord broken;

    for (const auto& branch : history->getBranches())
        if (branch.id == idea)
            broken = branch;

    REQUIRE (broken.working.isNotEmpty());
    broken.working = juce::String::repeatedString ("0", 64); // a piece the store hasn't
    REQUIRE (history->getStore()->writeBranch (broken));
    b.s.reopen (false);
    REQUIRE (waitForVersionHistory (b.s.context));

    CHECK (phazite::canDeleteBranch (b.s.context, idea)); // which finds out on the history's thread
    REQUIRE (waitForVersionHistory (b.s.context));
    CHECK (phazite::describeBranchDeletion (b.s.context, idea).contains ("can't be read"));
}
