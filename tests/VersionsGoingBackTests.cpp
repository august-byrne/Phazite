// SPDX-License-Identifier: AGPL-3.0-or-later
// Going back to a version (docs/versions.md: Going back, step V2): Bring Back This Version, Put Back and Take from This
// Version, each one undo step that what plays follows, with the fewest edits, and what isn't counted left as it is.
#include "GoldenFiles.h"
#include "RenderHarness.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/ExampleSongs.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/VersionHistory.h"

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

juce::String describeChanges (const std::vector<phazite::VersionChange>& changes)
{
    juce::StringArray lines;

    for (const auto& change : changes)
        lines.add ((change.kind == phazite::VersionChange::Kind::added     ? "+ "
                    : change.kind == phazite::VersionChange::Kind::removed ? "- "
                                                                           : "")
                   + change.name + (change.what.isNotEmpty() ? ": " + change.what : juce::String()));

    return lines.joinIntoString ("; ");
}

std::optional<phazite::VersionChange> findLine (const std::vector<phazite::VersionChange>& lines, const juce::String& name)
{
    for (const auto& line : lines)
        if (line.name == name)
            return line;

    return std::nullopt;
}

// Waits for the renders of audio clips that follow the tempo, or are pitched or reversed (TestSong::waitForClipRenders()).
void waitForClipRenders (phazite::ProjectContext& context)
{
    const auto anyPreparing = [&context]
    {
        for (const auto track : phazite::getAllPlaylistTracks (context))
            for (const auto& clip : phazite::getClips (context, track))
                if (const auto audio = phazite::getAudioClip (context, clip.id);
                    audio.has_value() && audio->state == phazite::AudioState::preparing)
                    return true;

        return false;
    };

    REQUIRE (dispatchMessagesUntil ([&anyPreparing] { return ! anyPreparing(); }, 30000));
}

// The differences between two saves of a project, but for the song's count of IDs, which no Bring Back takes back.
juce::StringArray findProjectDifferences (const juce::XmlElement& expected, const juce::XmlElement& actual)
{
    juce::StringArray differences;

    for (const auto& line : findDifferences (expected, actual))
        if (! line.contains ("@nextID"))
            differences.add (line);

    return differences;
}
} // namespace

TEST_CASE ("Bring Back makes the song what it was, as one undo step, and leaves its view state as it is")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto synth = s.setInstrument (bass, phazite::devices::Synth::typeID);
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    s.setVolume (bass, -6.0f);
    const auto verse = keepVersion (s.context, "Verse");
    auto& song = *phazite::findSong (s.context);
    const auto then = song.getState().createCopy();

    // Changes of each kind, and some view state.
    s.setVolume (bass, -12.0f);
    s.setParameter (synth, "cutoff", 900.0f);
    s.addNote (clip, 40, 2.0, 1.0);
    s.addEffect (bass, phazite::devices::Equalizer::typeID);
    s.addTrack ("Strings");
    s.setTempo (128.0);
    s.rename (bass, "Low");
    phazite::findTrackState (song, bass).setProperty (schema::trackHeight, 140, nullptr);
    const auto changed = song.getState().createCopy();
    REQUIRE (phazite::hasChangesSinceLastVersion (s.context));

    REQUIRE (phazite::bringBackVersion (s.context, verse));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (phazite::findChanges (then, song.getState()).empty());
    CHECK ((int) phazite::findTrackState (song, bass)[schema::trackHeight] == 140); // view state stays
    CHECK (phazite::describeUndo (&song) == "Undo Restore from \"Verse\"");
    CHECK (phazite::getVersions (s.context).size() == 2); // the history keeps every commit, the initial one too

    // Undo takes it all back, in one step.
    phazite::undo (song);
    CHECK (phazite::findChanges (changed, song.getState()).empty());
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
}

TEST_CASE ("Put Back takes one line back from the last version, and Take from This Version from another")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto keys = s.addTrack ("Keys");
    const auto clip = s.addMidiClip (keys, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0);
    s.setVolume (bass, -6.0f);
    const auto first = keepVersion (s.context, "First");

    s.setVolume (bass, -12.0f);
    keepVersion (s.context, "Second");

    // Changed, added and taken away since the last version: each put back on its own.
    s.setVolume (bass, -20.0f);
    s.addTrack ("Strings");
    phazite::deleteTracks (*phazite::findSong (s.context), { keys }, nullptr);
    CHECK (describeChanges (phazite::getListedChanges (s.context)) == "Bass: volume; + Strings; - Keys");

    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Bass")));
    CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-12.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard Changes in Bass");

    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Strings")));
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Keys")));
    CHECK (describeChanges (phazite::getListedChanges (s.context)).isEmpty());

    // Keys came back where it was, after Bass, with its clip and its note.
    const auto tracks = phazite::getTrackIDs (*phazite::findSong (s.context));
    REQUIRE (tracks.size() == 3);
    CHECK (tracks[1] == bass);
    CHECK (tracks[2] == keys);
    CHECK (phazite::getClips (*phazite::findSong (s.context), keys).size() == 1);

    // Take from This Version: what differs from the first version, and Bass taken from it.
    phazite::compareWithVersion (s.context, first);
    CHECK (phazite::getComparedVersion (s.context) == first);
    CHECK (describeChanges (phazite::getListedChanges (s.context)) == "Bass: volume");
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Bass")));
    CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Restore Bass from \"First\"");
    CHECK (describeChanges (phazite::getListedChanges (s.context)).isEmpty());

    // Back to the changes since the last version, which that one is now.
    phazite::compareWithVersion (s.context, {});
    CHECK (describeChanges (phazite::getListedChanges (s.context)) == "Bass: volume");
}

TEST_CASE ("The commands bring back the version selected, and put back the line selected")
{
    TestSong s;
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, s.context, [] (std::function<void (bool)> then) { then (true); }, s.renders);
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.setVolume (bass, -9.0f);

    CHECK_FALSE (commands.invoke ("version.bringBack")); // nothing selected
    phazite::selectVersion (s.context, first);
    CHECK (phazite::getCurrentName (*commands.find ("version.bringBack")) == "Restore from \"First\"");
    CHECK_FALSE (phazite::canTakeFrom (s.context)); // it is the last version: its changes are listed already
    REQUIRE (commands.invoke ("version.bringBack"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    s.setVolume (bass, -9.0f);
    phazite::selectChange (s.context, phazite::getListedChanges (s.context).front());
    REQUIRE (commands.invoke ("version.putBack"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (phazite::canPutBack (s.context)); // the line it put back is gone
}

TEST_CASE ("Bring Back sets a plug-in's settings only where they differ from the version's, and Undo puts them back")
{
    TestPlugin plugin;
    TestSong s;
    const auto track = s.addTrack ("Lead");
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (*phazite::findSong (s.context), "Add Plug-in");
        device = TestPlugin::add (*phazite::findSong (s.context), track);
    }

    REQUIRE (device.isValid());
    REQUIRE (
        dispatchMessagesUntil ([&s, device] { return phazite::findPluginInstance (*phazite::findSong (s.context), device) != nullptr; }));
    const auto gain = TestPlugin::getGain (*phazite::findSong (s.context), device);
    const auto version = keepVersion (s.context, "Before");

    // What writes the plug-in's settings into the song, which its instance then takes.
    struct StateWrites : public juce::ValueTree::Listener
    {
        int count = 0;
        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
        {
            if (tree.hasType (schema::DEVICE) && property == schema::pluginState)
                ++count;
        }
    };

    auto& song = *phazite::findSong (s.context);
    StateWrites writes;
    auto state = song.getState(); // a listener belongs to the tree object it was added to
    state.addListener (&writes);

    // Something else changed: the plug-in isn't set again.
    s.setVolume (track, -8.0f);
    writes.count = 0;
    REQUIRE (phazite::bringBackVersion (s.context, version));
    CHECK (writes.count == 0);

    // The plug-in's own settings changed in its window: they are set back to the version's, and Undo puts them back.
    TestPlugin::setGain (song, device, gain * 0.25f);
    REQUIRE (phazite::bringBackVersion (s.context, version));
    CHECK (writes.count >= 1);
    REQUIRE (dispatchMessagesUntil ([&song, device, gain] { return std::abs (TestPlugin::getGain (song, device) - gain) < 0.001f; }));

    phazite::undo (song);
    REQUIRE (
        dispatchMessagesUntil ([&song, device, gain] { return std::abs (TestPlugin::getGain (song, device) - gain * 0.25f) < 0.001f; }));
    state.removeListener (&writes);
}

TEST_CASE ("Every example song, changed through the model and brought back, saves and plays as it did")
{
    for (const auto& example : phazite::getExampleSongs())
    {
        INFO (juce::String (example.id).toStdString());
        TemporaryFolder temp;
        phazite::ProjectContext context { getEngineContext() };
        juce::String error;
        auto project = phazite::Project::create (
            temp.folder, example.id, error, phazite::getExampleSongContent (getEngineContext(), example.id, getFactoryPresetsFolder()));
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());
        phazite::setSong (context, &project->getSong());

        waitForClipRenders (context);
        const auto version = keepVersion (context);
        REQUIRE (project->save().wasOk());
        const auto saved = readNormalisedProject (project->getFile());
        const auto played = writeFingerprint (renderFingerprint (context, 2.0), "before");

        // A change of every kind, through the model, in one step.
        auto& song = project->getSong();
        const auto tracks = phazite::getTrackIDs (song);
        REQUIRE (tracks.size() >= 2);

        {
            const phazite::UndoStep step (song, "Changes");
            phazite::setChannelVolume (song, tracks[0], -9.0f);
            phazite::setTrackName (song, tracks[0], "Renamed");
            phazite::setTempo (song, 133.0);
            phazite::insertDeviceState (song, tracks[0], phazite::makeOwnDeviceState (song, phazite::devices::Equalizer::typeID));
            phazite::insertTrack (song, tracks[1], nullptr);

            for (const auto track : tracks)
            {
                if (const auto clips = phazite::getClips (song, track); ! clips.empty())
                {
                    if (phazite::findClipState (song, clips.front())[schema::clipKind].toString() == schema::clipIsMidi)
                        phazite::addNote (song, clips.front(), 64, phazite::BeatPosition::fromBeats (0.5),
                                          phazite::BeatDuration::fromBeats (0.25), 90);

                    phazite::deleteClips (song, { clips.back() }, false);
                    break;
                }
            }

            phazite::deleteTracks (song, { tracks.back() }, nullptr);
        }

        REQUIRE (phazite::hasChangesSinceLastVersion (context));
        REQUIRE (phazite::bringBackVersion (context, version));
        INFO (describeChanges (phazite::getChangesSinceLastVersion (context)).toStdString());
        CHECK_FALSE (phazite::hasChangesSinceLastVersion (context));

        waitForClipRenders (context);
        REQUIRE (project->save().wasOk());
        const auto differences = findProjectDifferences (*saved, *readNormalisedProject (project->getFile()));
        INFO (differences.joinIntoString ("\n").toStdString());
        CHECK (differences.isEmpty());

        const auto replayed = writeFingerprint (renderFingerprint (context, 2.0), "before");
        const auto renderDifferences = compareFingerprints (played, replayed);
        INFO (renderDifferences.joinIntoString ("\n").toStdString());
        CHECK (renderDifferences.isEmpty());

        phazite::setSong (context, nullptr);
    }
}
