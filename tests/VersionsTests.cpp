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

TEST_CASE ("A project keeps no versions until one is saved, then lists it on Main")
{
    TestSong s;
    waitForVersions (s.context);
    CHECK (phazite::keepsVersions (s.context));
    CHECK (phazite::getVersions (s.context).empty());
    CHECK_FALSE (phazite::getCurrentBranch (s.context).has_value());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context)); // nothing to be changed from yet
    CHECK_FALSE (s.getProject().getFolder().getChildFile ("Versions").exists());

    keepVersion (s.context, "First idea", "Drums only, for now");

    const auto versions = phazite::getVersions (s.context);
    REQUIRE (versions.size() == 1);
    CHECK (versions[0].name == "First idea");
    CHECK (versions[0].note == "Drums only, for now");
    CHECK (versions[0].hasOwnName);
    CHECK (versions[0].parents.isEmpty());

    const auto branch = phazite::getCurrentBranch (s.context);
    REQUIRE (branch.has_value());
    CHECK (branch->name == "Main");
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

    // Saved without a name, the version is named by what changed.
    keepVersion (s.context);
    CHECK (phazite::getVersions (s.context).front().name == "Bass, + Strings, Tempo");
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
    REQUIRE (versions.size() == 2);
    CHECK (versions[0].name == "Two");
    CHECK (versions[1].name == "One");
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

TEST_CASE ("Save Version saves the project first, then records the version")
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
        [&s] (const juce::ValueTree& song) { s.openSong (song); });

    waitForVersions (s.context);
    s.addTrack ("Lead");
    REQUIRE (s.getProject().hasUnsavedChanges());
    REQUIRE (commands.invoke ("version.save"));
    waitForVersions (s.context);

    CHECK (saves == 1);
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (phazite::getVersions (s.context).size() == 1);
    CHECK_FALSE (phazite::getVersions (s.context).front().hasOwnName);
    CHECK (phazite::getVersions (s.context).front().name == "First version");
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
    REQUIRE (versions.size() == 1);
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

TEST_CASE ("A version without a name is named by its changes, a few of them")
{
    using Change = phazite::SongChange;
    const std::vector<Change> changes { { Change::Where::track, Change::Kind::changed, 1, "Bass", { "volume" } },
                                        { Change::Where::track, Change::Kind::changed, 2, "Drums", { "2 clips added" } },
                                        { Change::Where::track, Change::Kind::added, 3, "Strings", {} },
                                        { Change::Where::track, Change::Kind::removed, 4, "Pad", {} },
                                        { Change::Where::song, Change::Kind::changed, 0, "Song", { "tempo", "sections" } } };

    CHECK (phazite::summarise ({ changes.begin(), changes.begin() + 3 }) == "Bass, Drums, + Strings");
    CHECK (phazite::summarise (changes) == "Bass, Drums, + Strings, - Pad, 2 more");
    CHECK (phazite::summarise ({}) == "No changes");
}
