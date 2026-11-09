// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's versions (docs/versions.md): saving them, reading them back as the project opens, and the changes since the
// last one, by track, in Phazite's own words.
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/modulation/Modulators.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
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

TEST_CASE ("A knob turned and set back to its default, or a modulator's, is no change since the last version, nor unsaved")
{
    // As a double-click sets a knob back (known bug 9 until 2026-11-06: the default stayed in the device's values).
    TestSong s;
    const auto track = s.addTrack ("Keys");
    const auto compressor = s.addEffect (track, phazite::devices::Compressor::typeID);
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    const auto follower = s.addModulator (track, phazite::ModulatorType::envelopeFollower);
    const auto defaultOf = [] (const auto& parameters, std::string_view parameter)
    {
        for (const auto& info : parameters)
            if (info.id == parameter)
                return info.defaultValue;

        FAIL ("no parameter " << parameter);
        return 0.0f;
    };
    const auto& compressorParameters = phazite::devices::Compressor::info.parameters;
    s.setParameter (compressor, "attack", 30.0f); // a value set before the version, which stays as it was
    keepVersion (s.context);
    REQUIRE (s.getProject().save().wasOk());
    dispatchPendingMessages();
    REQUIRE_FALSE (s.getProject().hasUnsavedChanges());
    auto& song = *phazite::findSong (s.context);
    const auto values = [&song, compressor] { return phazite::findDeviceState (song, compressor).getChildWithName (schema::VALUES); };
    REQUIRE (values().getNumProperties() == 1);

    s.setParameter (compressor, "inputGain", 6.0f);
    CHECK (describeChanges (s.context) == "Keys: Compressor");
    s.setParameter (compressor, "inputGain", defaultOf (compressorParameters, "inputGain"));
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (values().getNumProperties() == 1);

    // Its mix too; and the value set before the version set back, which leaves the device no values at all, as it was made.
    phazite::setMix (song, compressor, 0.5f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::setMix (song, compressor, 1.0f);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    s.setParameter (compressor, "attack", defaultOf (compressorParameters, "attack"));
    CHECK_FALSE (values().isValid());
    CHECK (describeChanges (s.context) == "Keys: Compressor");
    phazite::undo (song);
    CHECK ((float) values()["attack"] == doctest::Approx (30.0f));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // A modulator's parameter: an LFO's rate, and back; and an envelope follower's depth, which it starts at, its default.
    s.setModulatorParameter (lfo, "rate", 5.0f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    s.setModulatorParameter (lfo, "rate",
                             defaultOf (phazite::devices::getModulatorParameters (phazite::devices::ModulatorKind::lfo), "rate"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    s.setModulatorParameter (follower, "depth", 0.5f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    s.setModulatorParameter (
        follower, "depth",
        defaultOf (phazite::devices::getModulatorParameters (phazite::devices::ModulatorKind::envelopeFollower), "depth"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
}

TEST_CASE ("A track muted and unmuted again, soloed and unsoloed, or a clip muted and unmuted, is no change since the last version")
{
    TestSong s;
    const auto track = s.addTrack ("Keys");
    const auto other = s.addTrack ("Pad");
    const auto clip = s.addMidiClip (track, 0.0, 4.0);
    keepVersion (s.context);
    auto& song = *phazite::findSong (s.context);

    // Muted, it is a change; unmuted, the track is as the version has it, so the change goes (it stayed: 2026-11-06).
    phazite::toggleTracksMuted (song, { track });
    CHECK (describeChanges (s.context) == "Keys: mute");
    phazite::toggleTracksMuted (song, { track });
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // So do a solo, alone or not, and a clip's mute.
    phazite::toggleTracksSoloed (song, { track });
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleTracksSoloed (song, { track });
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleSoloAlone (song, other);
    phazite::toggleSoloAlone (song, other);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleClipsMuted (song, { clip });
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleClipsMuted (song, { clip });
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
}

TEST_CASE ("Collapse All and Expand All, the master's row and lanes with the rest, are no change since the last version, nor unsaved")
{
    // The owner, 2026-11-06: Alt+U (Collapse All) in the playlist left a change of the master's, which Expand All didn't
    // take away.
    TestSong s;
    s.addTrack ("Keys");
    auto& song = *phazite::findSong (s.context);
    const auto master = phazite::getMasterID (song);
    phazite::showLane (song, master, { phazite::getFader (song, master), phazite::getVolumeParameterID (song, master) });
    keepVersion (s.context);
    REQUIRE (s.getProject().save().wasOk());
    dispatchPendingMessages();
    REQUIRE_FALSE (s.getProject().hasUnsavedChanges());

    s.run ("track.collapseAll");
    REQUIRE (phazite::isCollapsed (song, master));
    REQUIRE_FALSE (phazite::isAnyRowExpanded (song));
    dispatchPendingMessages();
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    s.run ("track.collapseAll");
    REQUIRE_FALSE (phazite::isCollapsed (song, master));
    dispatchPendingMessages();
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
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

TEST_CASE ("A plug-in's stored settings that differ count as a change only after it said it changed, or a knob of it moved")
{
    phazite::test::TestPlugin testPlugin; // known while the song is
    TestSong s;
    const auto track = s.addTrack ("Effects");
    auto& song = *phazite::findSong (s.context);
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add the stand-in");
        device = phazite::test::TestPlugin::add (song, track);
    }

    keepVersion (s.context, "First");
    CHECK (describeChanges (s.context).isEmpty());

    // Other bytes stored with nothing said, as a plug-in that writes a time into its state leaves them at each save.
    phazite::findDeviceState (song, device)
        .setProperty (schema::pluginState, phazite::writeBytes (juce::MemoryBlock ("other", 5)), nullptr);
    dispatchPendingMessages();
    CHECK (describeChanges (s.context).isEmpty());

    // Once it said its settings changed, they count.
    song.markChangedOutsideHistory (device.getRaw());
    CHECK (describeChanges (s.context).contains ("Effects"));
}

TEST_CASE ("A plug-in's update shows in the changes, by the version that made its settings then and now")
{
    phazite::test::TestPlugin testPlugin;
    TestSong s;
    const auto track = s.addTrack ("Effects");
    auto& song = *phazite::findSong (s.context);
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add the stand-in");
        device = phazite::test::TestPlugin::add (song, track);
    }

    // A commit stores the version of the plug-in that made the state, as it stores the state.
    phazite::findDeviceState (song, device).setProperty (schema::pluginVersion, "1.0.0", nullptr);
    keepVersion (s.context, "First");
    const auto then = phazite::findDeviceState (song, device)[schema::pluginVersion].toString();
    const auto installed = phazite::test::TestPlugin::getDescription().version;
    CHECK (then == (installed.isNotEmpty() ? installed : juce::String ("1.0.0")));

    phazite::findDeviceState (song, device).setProperty (schema::pluginVersion, "9.9.9", nullptr);
    CHECK (describeChanges (s.context).contains ("updated " + then + juce::String::fromUTF8 (" \xe2\x86\x92 9.9.9")));
}

TEST_CASE ("Verify History finds the commits no branch holds and what is damaged, and Recover Lost Commits makes a branch of each line")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto* history = phazite::getVersionHistory (s.context);
    const auto first = history->findLastVersion()->id;

    // A commit that no branch holds, as a lost branch file would leave one: written into the store beside the others.
    juce::String lostID;

    {
        phazite::VersionStore store (phazite::getVersionsFolder (song), "elsewhere");
        phazite::VersionRecord lost;
        lost.name = "Lost";
        lost.parents.add (first);
        lost.branch = history->findCurrentBranch()->id;
        lost.saved = juce::Time::currentTimeMillis();
        lostID = store.writeVersion (lost, phazite::splitSong (song.getState()));
        REQUIRE (lostID.isNotEmpty());
    }

    const auto verify = [&s]
    {
        REQUIRE (phazite::verifyHistory (s.context));
        REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isVerifyingHistory (s.context); }, 20000));
    };

    verify();
    CHECK (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    CHECK (phazite::countLostVersions (s.context) == 1);
    CHECK (phazite::describeHistoryCheck (s.context).contains ("Recover Lost Commits"));

    // Recovered: a branch at it, named after the branch it was made on.
    CHECK (phazite::recoverLostVersions (s.context) == 1);
    CHECK (phazite::countLostVersions (s.context) == 0);
    const auto branches = phazite::getBranches (s.context);
    CHECK (std::any_of (branches.begin(), branches.end(), [&lostID] (const phazite::BranchInfo& branch)
                        { return branch.name == "main-recovered" && branch.head == lostID; }));

    // A byte of a pack damaged: the piece is found, and the commits that need it named.
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Second");
    juce::File newest;

    for (const auto& pack :
         phazite::getVersionsFolder (song).getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"))
        if (newest == juce::File() || pack.getLastModificationTime() > newest.getLastModificationTime())
            newest = pack;

    juce::MemoryBlock bytes;
    REQUIRE (newest.loadFileAsData (bytes));
    static_cast<char*> (bytes.getData())[10] ^= 0x5a;
    REQUIRE (newest.replaceWithData (bytes.getData(), bytes.getSize()));

    verify();
    CHECK (phazite::describeHistoryCheck (s.context).contains ("damaged"));
}

TEST_CASE ("A branch another computer took its own way keeps this computer's line, with the other's as divergent")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto* history = phazite::getVersionHistory (s.context);
    const auto first = history->findLastVersion()->id;
    const auto main = *history->findCurrentBranch();

    // Another computer, from the same commit, committed on main too: its own record of main says so.
    juce::String theirs;

    {
        phazite::VersionStore elsewhere (phazite::getVersionsFolder (song), "elsewhere");
        phazite::VersionRecord record;
        record.name = "Theirs";
        record.parents.add (first);
        record.branch = main.id;
        record.saved = juce::Time::currentTimeMillis();
        theirs = elsewhere.writeVersion (record, phazite::splitSong (song.getState()));
        REQUIRE (theirs.isNotEmpty());

        auto branch = main;
        branch.head = theirs;
        branch.computer = "elsewhere";
        branch.working = {};
        REQUIRE (elsewhere.writeBranch (branch));
    }

    // This computer commits its own; then the history read again, as when the project next opens.
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Mine");
    const auto mine = history->findLastVersion()->id;

    phazite::VersionHistory reread (song, {});
    REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }));
    REQUIRE (reread.findCurrentBranch() != nullptr);
    CHECK (reread.findCurrentBranch()->head == mine);
    CHECK (reread.findCurrentBranch()->divergent == juce::StringArray (theirs));
}

TEST_CASE ("A project's last commit is read from its Versions folder without the project, to open in its place")
{
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "Kept");
    auto& song = *phazite::findSong (s.context);

    const auto [commit, name] = phazite::readLastCommitSong (song.getFile());
    REQUIRE (commit.isValid());
    CHECK (name == "Kept");
    CHECK (commit.hasType (schema::PHAZITE));
    CHECK (phazite::getTrackElements (commit).size() == phazite::getTrackElements (song.getState()).size());
}
