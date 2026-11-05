// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's versions (docs/versions.md): saving them, reading them back as the project opens, and the changes since the
// last one, by track, in Phazite's own words.
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/VersionHistory.h"

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isBusy();
        }));
}

// Saves a version with a name and a note, and waits until it is written.
void keepVersion (phazite::ProjectContext& context, const juce::String& name = {}, const juce::String& note = {})
{
    waitForVersions (context);
    phazite::setVersionDraft (context, name, note);
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }));
    INFO (result->getErrorMessage().toStdString());
    REQUIRE (result->wasOk());
    waitForVersions (context);
}

juce::String describe (const std::vector<phazite::VersionChange>& changes)
{
    juce::StringArray lines;

    for (const auto& change : changes)
        lines.add ((change.kind == phazite::VersionChange::Kind::added     ? "+ "
                    : change.kind == phazite::VersionChange::Kind::removed ? "- "
                                                                           : "")
                   + change.name + (change.what.isNotEmpty() ? ": " + change.what : juce::String()));

    return lines.joinIntoString ("; ");
}

juce::String describeChanges (phazite::ProjectContext& context)
{
    return describe (phazite::getChangesSinceLastVersion (context));
}
} // namespace

TEST_CASE ("A project starts with its initial commit on main, and lists each commit after it")
{
    TestSong s;
    waitForVersions (s.context);
    CHECK (phazite::keepsVersions (s.context));
    const auto initial = phazite::getVersions (s.context);
    REQUIRE (initial.size() == 1);
    CHECK (initial[0].name == "Initial commit");
    CHECK (initial[0].parents.isEmpty());
    CHECK (phazite::getCurrentBranch (s.context)->name == "main");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (s.getProject().getFolder().getChildFile ("Versions").isDirectory());

    keepVersion (s.context, "First idea", "Drums only, for now");

    const auto versions = phazite::getVersions (s.context);
    REQUIRE (versions.size() == 2);
    CHECK (versions[0].name == "First idea");
    CHECK (versions[0].note == "Drums only, for now");
    CHECK (versions[0].hasOwnName);
    CHECK (versions[0].parents == juce::StringArray (initial[0].id));

    const auto branch = phazite::getCurrentBranch (s.context);
    REQUIRE (branch.has_value());
    CHECK (branch->name == "main");
    CHECK (branch->head == versions[0].id);
    CHECK (versions[0].branch == branch->id);
    CHECK (phazite::getLastVersion (s.context)->id == versions[0].id);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (phazite::getVersionDraftName (s.context).isEmpty()); // the draft went with the version
    CHECK (phazite::getVersionsSize (s.context) > 0);
}

TEST_CASE ("The changes since the last version name each track's, in Phazite's own words")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto synth = s.setInstrument (bass, phazite::devices::Synth::typeID);
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    s.addNote (clip, 38, 1.0, 1.0);
    keepVersion (s.context, "Verse");
    CHECK (describeChanges (s.context).isEmpty());

    s.setVolume (bass, -6.0f);
    CHECK (describeChanges (s.context) == "Bass: volume");

    s.addNote (clip, 40, 2.0, 1.0);
    s.addNote (clip, 41, 3.0, 1.0);
    CHECK (describeChanges (s.context) == "Bass: volume, 2 notes");

    s.setParameter (synth, "cutoff", 1200.0f);
    CHECK (describeChanges (s.context) == "Bass: volume, Synth, 2 notes");

    const auto eq = s.addEffect (bass, phazite::devices::Equalizer::typeID);
    CHECK (describeChanges (s.context) == "Bass: volume, Synth, + EQ, 2 notes");

    const auto strings = s.addTrack ("Strings");
    CHECK (describeChanges (s.context) == "Bass: volume, Synth, + EQ, 2 notes; + Strings");

    s.addAutomation ({ eq, "band1Gain" }, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (4.0), 0.7f } });
    CHECK (describeChanges (s.context).startsWith ("Bass: volume, Synth, + EQ, 2 notes, automation"));

    s.setTempo (128.0);
    CHECK (describeChanges (s.context).endsWith ("; Song: tempo"));

    // Committed without a message, the commit is named by what changed, as git's messages are written.
    keepVersion (s.context);
    CHECK (phazite::getVersions (s.context).front().name == "Add Strings; update Bass and tempo");
    CHECK_FALSE (phazite::getVersions (s.context).front().hasOwnName);
    CHECK (describeChanges (s.context).isEmpty());
    juce::ignoreUnused (strings);
}

TEST_CASE ("View state and how the song plays are no change since the last version")
{
    TestSong s;
    const auto track = s.addTrack ("Keys");
    keepVersion (s.context);

    auto& song = *phazite::findSong (s.context);
    auto state = phazite::findTrackState (song, track);
    state.setProperty (schema::trackHeight, 120, nullptr);
    state.setProperty (schema::trackCollapsed, true, nullptr);
    song.getState().setProperty (schema::loop, true, nullptr);
    song.getState().setProperty (schema::metronome, true, nullptr);
    song.getState().setProperty (schema::gridStep, "1 bar", nullptr);

    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // An undo back to where the version was is no change either.
    s.setVolume (track, -3.0f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::undo (song);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
}

TEST_CASE ("A project's versions are read again as it opens, with the song each one kept")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "One");
    const auto firstSong = phazite::findSong (s.context)->getState().createCopy();

    s.setVolume (bass, -10.0f);
    keepVersion (s.context, "Two");
    s.setVolume (bass, -20.0f); // since the last version, and saved with the project

    s.reopen();
    waitForVersions (s.context);

    const auto versions = phazite::getVersions (s.context);
    REQUIRE (versions.size() == 3);
    CHECK (versions[0].name == "Two");
    CHECK (versions[1].name == "One");
    CHECK (versions[2].name == "Initial commit");
    CHECK (versions[0].parents == juce::StringArray (versions[1].id));
    CHECK (phazite::getCurrentBranch (s.context)->head == versions[0].id);
    CHECK (describeChanges (s.context) == "Bass: volume");

    // The first version's song, as it was.
    auto* history = phazite::getVersionHistory (s.context);
    REQUIRE (history != nullptr);
    const auto read = history->readSong (versions[1].id);
    REQUIRE (read.isValid());
    CHECK (phazite::holdTheSame (read, firstSong));
    CHECK (phazite::findChanges (read, firstSong).empty());
}

TEST_CASE ("Commit saves the project first, then commits")
{
    TestSong s;
    phazite::CommandRegistry commands;
    int saves = 0;

    phazite::addVersionCommands (
        commands, s.context,
        [&s, &saves] (std::function<void (bool)> then)
        {
            ++saves;
            then (s.getProject().save().wasOk());
        },
        s.renders);

    waitForVersions (s.context);
    s.addTrack ("Lead");
    REQUIRE (s.getProject().hasUnsavedChanges());
    REQUIRE (commands.invoke ("version.save"));
    waitForVersions (s.context);

    CHECK (saves == 1);
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (phazite::getVersions (s.context).size() == 2);
    CHECK_FALSE (phazite::getVersions (s.context).front().hasOwnName);
    CHECK (phazite::getVersions (s.context).front().name == "Add Lead");
}

TEST_CASE ("A copy of a project takes its versions with it")
{
    TestSong s;
    s.addTrack ("Pad");
    keepVersion (s.context, "Before the copy");

    juce::String error;
    const auto copy = s.getProject().saveCopyAs (s.getProject().getFolder().getParentDirectory(), "Copy", error);
    INFO (error.toStdString());
    REQUIRE (copy.existsAsFile());

    phazite::ProjectContext context { getEngineContext() };
    auto project = phazite::Project::open (copy, false, error);
    REQUIRE (project != nullptr);
    phazite::setSong (context, &project->getSong());
    waitForVersions (context);

    const auto versions = phazite::getVersions (context);
    REQUIRE (versions.size() == 2);
    CHECK (versions[0].name == "Before the copy");
    phazite::setSong (context, nullptr);
}

//==============================================================================
TEST_CASE ("The differences between two songs match items by their IDs, wherever they went")
{
    using phazite::findChanges;
    auto before = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*before, 2);
    phazite::setTrackName (*before, tracks[0], "Drums");
    const auto then = before->getState().createCopy();

    // The second track moved above the first: the song's order, no track's own change.
    auto list = phazite::getTrackList (*before);
    const auto second = phazite::findTrackState (*before, tracks[1]);
    list.moveChild (list.indexOf (second), 0, nullptr);
    const auto moved = findChanges (then, before->getState());
    REQUIRE (moved.size() == 1);
    CHECK (moved[0].where == phazite::SongChange::Where::song);
    CHECK (moved[0].what == juce::StringArray ("track order"));

    // The same song: nothing.
    CHECK (findChanges (then, then.createCopy()).empty());
    CHECK (phazite::holdTheSame (then, then.createCopy()));

    // A plug-in's knobs named by the values it told.
    const phazite::PluginParameterValues valuesThen { { 7, { { "1", "Cutoff", 0.4f }, { "2", "Drive", 0.1f } } } };
    const phazite::PluginParameterValues valuesNow { { 7, { { "1", "Cutoff", 0.55f }, { "2", "Drive", 0.1f } } } };
    const juce::ValueTree plugin (
        schema::DEVICE, { { schema::deviceID, 7 }, { schema::deviceType, schema::deviceIsPlugin }, { schema::pluginName, "Serum" } });
    const juce::ValueTree trackThen (schema::TRACK, { { schema::trackID, 3 }, { schema::trackName, "Lead" } }, { plugin.createCopy() });
    const juce::ValueTree trackNow (schema::TRACK, { { schema::trackID, 3 }, { schema::trackName, "Lead" } }, { plugin.createCopy() });
    const auto change = phazite::findTrackChange (trackThen, trackNow, {}, {}, { &valuesThen, &valuesNow });
    CHECK (change.what == juce::StringArray (juce::String ("Serum: Cutoff 40%") + juce::String::fromUTF8 (" \xe2\x86\x92 ") + "55%"));
}

TEST_CASE ("A commit without a message gets one as git's are written, from its changes, a few of them")
{
    using Change = phazite::SongChange;
    const std::vector<Change> changes { { Change::Where::track, Change::Kind::changed, 1, "Bass", { "volume" } },
                                        { Change::Where::track, Change::Kind::changed, 2, "Drums", { "2 clips added" } },
                                        { Change::Where::track, Change::Kind::added, 3, "Strings", {} },
                                        { Change::Where::track, Change::Kind::removed, 4, "Pad", {} },
                                        { Change::Where::song, Change::Kind::changed, 0, "Song", { "tempo", "sections" } } };

    CHECK (phazite::summarise ({ changes.begin(), changes.begin() + 3 }) == "Add Strings; update Bass and Drums");
    CHECK (phazite::summarise (changes) == "Add Strings; update Bass, Drums, tempo and sections; remove Pad");
    CHECK (phazite::summarise ({}) == "No changes");
}

TEST_CASE ("Staging, as git's: the staged changes are what a commit records, and the changes what changed since they were staged")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    keepVersion (s.context, "Start");
    const auto volume = [&s] (phazite::TrackID track) { return phazite::getChannelVolume (*phazite::findSong (s.context), track); };

    s.setVolume (bass, -6.0f);
    s.setVolume (drums, -9.0f);
    CHECK (describeChanges (s.context) == "Bass: volume; Drums: volume");
    CHECK (phazite::getStagedChanges (s.context).empty());
    CHECK (phazite::countChanges (s.context) == 2);

    // Bass staged: in the staged changes, no more in the changes; Drums still to stage.
    REQUIRE (phazite::stageChange (s.context, phazite::getChangesSinceLastVersion (s.context)[0]));
    auto staged = phazite::getStagedChanges (s.context);
    REQUIRE (staged.size() == 1);
    CHECK (staged[0].name == "Bass");
    CHECK (staged[0].staged);
    CHECK (describeChanges (s.context) == "Drums: volume");
    CHECK (phazite::countChanges (s.context) == 2);

    // Changed again once staged: in both, as in git; Discard Changes takes it back to what is staged, as git restore does.
    s.setVolume (bass, -3.0f);
    CHECK (describeChanges (s.context) == "Bass: volume; Drums: volume");
    REQUIRE (phazite::takeBack (s.context, phazite::getChangesSinceLastVersion (s.context)[0]));
    CHECK (volume (bass) == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard Changes in Bass");
    CHECK (describeChanges (s.context) == "Drums: volume");

    // A commit records what is staged alone: Drums stays a change since it.
    auto* history = phazite::getVersionHistory (s.context);
    const auto start = phazite::getLastVersion (s.context)->id;
    keepVersion (s.context, "Bass down");
    CHECK (phazite::getStagedChanges (s.context).empty());
    CHECK (describeChanges (s.context) == "Drums: volume");
    CHECK (describe (phazite::getChangesSinceLastVersion (s.context)) == "Drums: volume");
    const auto committed = phazite::findChanges (history->readSong (start), history->readSong (phazite::getLastVersion (s.context)->id));
    REQUIRE (committed.size() == 1);
    CHECK (committed[0].name == "Bass");

    // Unstaged again: the line back in the changes; every line at once.
    REQUIRE (phazite::stageChange (s.context, phazite::getChangesSinceLastVersion (s.context)[0]));
    REQUIRE (phazite::unstageChange (s.context, phazite::getStagedChanges (s.context)[0]));
    CHECK (phazite::getStagedChanges (s.context).empty());
    CHECK (describeChanges (s.context) == "Drums: volume");
    phazite::stageAllChanges (s.context);
    CHECK (phazite::getChangesSinceLastVersion (s.context).empty());
    CHECK (phazite::getStagedChanges (s.context).size() == 1);
    phazite::unstageAllChanges (s.context);
    CHECK (phazite::getStagedChanges (s.context).empty());

    // With nothing staged, a commit takes every change, as git's smart commit.
    keepVersion (s.context, "Drums down");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // Discard All Changes: the song as committed again, one undo step.
    s.setVolume (bass, 0.0f);
    s.addTrack ("Keys");
    REQUIRE (phazite::discardAllChanges (s.context));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard All Changes");
}

TEST_CASE ("A branch's name is what git takes: spaces as dashes, without what a ref can't hold")
{
    CHECK (phazite::toBranchName ("Half time") == "Half-time");
    CHECK (phazite::toBranchName ("  big   chorus ") == "big-chorus");
    CHECK (phazite::toBranchName ("what?*[now]:~^") == "whatnow]");
    CHECK (phazite::toBranchName ("../.hidden.lock") == "hidden");
    CHECK (phazite::toBranchName ("feature//new idea/") == "feature/new-idea");
    CHECK (phazite::toBranchName ("---") == "");
}
