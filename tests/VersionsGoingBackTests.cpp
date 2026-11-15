// SPDX-License-Identifier: AGPL-3.0-or-later
// Going back to a version (docs/design/versions.md: Going back, step V2): Bring Back This Version, Put Back and Take from This
// Version, each one undo step that what plays follows, with the fewest edits, and what isn't counted left as it is.
#include "GoldenFiles.h"
#include "RenderHarness.h"
#include "SongIDs.h"
#include "TestClips.h"
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
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"
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

    REQUIRE (dispatchMessagesUntil ([&anyPreparing] { return ! anyPreparing(); }));
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

TEST_CASE (
    "Discard Changes takes a folder added since away without the tracks in it, and brings one taken away since back with those not elsewhere")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto keys = s.addTrack ("Keys");
    const auto clip = s.addMidiClip (keys, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0);
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto changes = [&s] { return describeChanges (phazite::getListedChanges (s.context)); };

    // Grouped into a folder since: the folder's line discarded, its tracks are where they were, with what they hold.
    phazite::TrackID folder;

    {
        const phazite::UndoStep step (song, "Group");
        folder = phazite::groupIntoFolder (song, { bass, keys }, nullptr);
    }

    REQUIRE (findLine (phazite::getListedChanges (s.context), "Folder").has_value());
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Folder")));
    checkIDs (song);
    CHECK_FALSE (phazite::findTrackState (song, folder).isValid());
    CHECK (phazite::getTrackIDs (song) == std::vector { s.firstTrack, bass, keys });
    CHECK (phazite::getClips (song, keys) == std::vector { clip });
    CHECK (changes().isEmpty());

    // One undo step: the folder again, with its tracks.
    phazite::undo (song);
    CHECK (phazite::getTracksInFolder (song, folder) == std::vector { bass, keys });
    checkIDs (song);
    keepVersion (s.context, "Grouped");

    // Ungrouped since: the folder's line discarded, it is back without its tracks, which are elsewhere; then the song's own,
    // their order, and they are in it again.
    {
        const phazite::UndoStep step (song, "Ungroup");
        phazite::ungroupFolder (song, folder);
    }

    REQUIRE (findLine (phazite::getListedChanges (s.context), "Folder").has_value());
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Folder")));
    checkIDs (song);
    CHECK (phazite::getTrackIDs (song).size() == 4);
    CHECK (phazite::getTracksInFolder (song, folder).empty());
    REQUIRE (findLine (phazite::getListedChanges (s.context), "Song").has_value());
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Song")));
    checkIDs (song);
    CHECK (phazite::getTracksInFolder (song, folder) == std::vector { bass, keys });
    CHECK (changes().isEmpty());

    // Deleted since with its tracks: the folder's line discarded, it is back with them, each once.
    {
        const phazite::UndoStep step (song, "Delete");
        phazite::deleteTracks (song, { folder }, nullptr);
    }

    REQUIRE (phazite::getTrackIDs (song).size() == 1);
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Folder")));
    checkIDs (song);
    CHECK (phazite::getTracksInFolder (song, folder) == std::vector { bass, keys });
    CHECK (phazite::getClips (song, keys) == std::vector { clip });
    CHECK (changes().isEmpty());
}

TEST_CASE ("A track's line discarded or restored brings a clip moved off the track since back from where it is now, once in the song")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto keys = s.addTrack ("Keys");
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    const auto first = keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto changes = [&s] { return describeChanges (phazite::getListedChanges (s.context)); };

    const auto moveToKeys = [&song, clip, keys]
    {
        const phazite::UndoStep step (song, "Move Clip");
        phazite::moveClip (song, clip, phazite::BeatPosition::fromBeats (2.0), keys);
    };

    // Moved onto Keys since. Discard Changes in Bass: the clip is on Bass again, and on Keys no more.
    moveToKeys();
    CHECK (changes() == "Bass: 1 clip removed; Keys: 1 clip added");
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Bass")));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    CHECK (changes().isEmpty());

    // One undo step: on Keys again, once.
    phazite::undo (song);
    checkIDs (song);
    CHECK (phazite::getClips (song, bass).empty());
    CHECK (phazite::getClips (song, keys) == std::vector { clip });

    // Keys' line staged alone: what is staged holds the clip once too, on Keys.
    REQUIRE (phazite::stageChange (s.context, *findLine (phazite::getListedChanges (s.context), "Keys")));
    checkUniqueIDs (phazite::getVersionHistory (s.context)->getStagedSong());
    phazite::unstageAllChanges (s.context);

    // Committed on Keys, then Bass restored from the commit before: the clip is on Bass, once.
    keepVersion (s.context, "Moved");
    const auto bassLine = findLine (phazite::getVersionChanges (s.context, phazite::getLastVersion (s.context)->id), "Bass");
    REQUIRE (bassLine.has_value());
    REQUIRE (phazite::restoreFromVersion (s.context, first, *bassLine));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    phazite::undo (song);

    // Bass deleted since, after its clip went onto Keys (the last commit has both so): Bass back, with its clip, once.
    REQUIRE (phazite::bringBackVersion (s.context, first));
    keepVersion (s.context, "As at first");
    moveToKeys();

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { bass }, nullptr);
    }

    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Bass")));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    CHECK (changes().isEmpty());

    // Discard All Changes, after the clip went onto Keys and both tracks into a folder: all as it was, each once.
    moveToKeys();

    {
        const phazite::UndoStep step (song, "Group");
        phazite::groupIntoFolder (song, { bass, keys }, nullptr);
    }

    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
    REQUIRE (phazite::discardAllChanges (s.context));
    REQUIRE (waitForVersionHistory (s.context));
    checkIDs (song);
    CHECK (phazite::getTrackIDs (song) == std::vector { s.firstTrack, bass, keys });
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (changes().isEmpty());
}

TEST_CASE ("The line of the track a clip was moved onto, discarded or restored alone, sends the clip back to the track it came from")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto keys = s.addTrack ("Keys");
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    const auto first = keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto changes = [&s] { return describeChanges (phazite::getListedChanges (s.context)); };

    const auto moveTo = [&song, clip] (phazite::TrackID track)
    {
        const phazite::UndoStep step (song, "Move Clip");
        phazite::moveClip (song, clip, phazite::BeatPosition::fromBeats (0.0), track);
    };

    // Moved onto Keys since, and a note added to it there. Discard Changes in Keys alone: the clip is on Bass again, where
    // the last commit has it, as it is now, and on Keys no more: on neither track would lose it.
    moveTo (keys);
    s.addNote (clip, 40, 2.0, 1.0);
    CHECK (changes() == "Bass: 1 clip removed; Keys: 1 clip added");
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Keys")));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    CHECK (changes() == "Bass: 1 note");

    // One undo step: on Keys again, once.
    phazite::undo (song);
    checkIDs (song);
    CHECK (phazite::getClips (song, bass).empty());
    CHECK (phazite::getClips (song, keys) == std::vector { clip });

    // Committed on Keys, then Keys alone restored from the commit before: the clip is on Bass, once.
    keepVersion (s.context, "Moved");
    const auto keysLine = findLine (phazite::getVersionChanges (s.context, phazite::getLastVersion (s.context)->id), "Keys");
    REQUIRE (keysLine.has_value());
    REQUIRE (phazite::restoreFromVersion (s.context, first, *keysLine));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    phazite::undo (song);

    // Moved onto a track added since: that track's line discarded, the track goes, and the clip is on Keys again.
    const auto strings = s.addTrack ("Strings");
    moveTo (strings);
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (s.context), "Strings")));
    checkIDs (song);
    CHECK_FALSE (phazite::findTrackState (song, strings).isValid());
    CHECK (phazite::getClips (song, keys) == std::vector { clip });
    CHECK (changes().isEmpty());
}

TEST_CASE ("What is restored into a song opened again as it was saved before it keeps its IDs: the next item made gets one of its own")
{
    // A project saved, then more made in it that Local History and a commit keep, but no save: the file counts its IDs from
    // before them (Don't Save, or a crash).
    const auto makeAndReopen = [] (TestSong& s, phazite::TrackID& keys, bool commit)
    {
        s.addTrack ("Bass");
        keepVersion (s.context, "First");
        REQUIRE (s.getProject().save().wasOk());
        const auto saved = phazite::findSong (s.context)->getState().createCopy();

        keys = s.addTrack ("Keys");
        s.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto clip = s.addMidiClip (keys, 0.0, 4.0);
        s.addNote (clip, 60, 0.0, 1.0);

        if (commit)
        {
            keepVersion (s.context, "Keys");
        }
        else
        {
            phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
            REQUIRE (waitForVersionHistory (s.context));
        }

        s.openSong (saved);
        REQUIRE (waitForVersionHistory (s.context));
        REQUIRE_FALSE (phazite::findTrackState (*phazite::findSong (s.context), keys).isValid());
    };

    const auto checkNextItems = [] (TestSong& s, phazite::TrackID keys)
    {
        auto& song = *phazite::findSong (s.context);
        REQUIRE (phazite::findTrackState (song, keys).isValid());
        checkIDs (song);

        const auto strings = s.addTrack ("Strings");
        s.setInstrument (strings, phazite::devices::Synth::typeID);
        s.addNote (s.addMidiClip (strings, 0.0, 4.0), 64, 0.0, 1.0);
        checkIDs (song);
    };

    // Local History restored.
    {
        TestSong s;
        phazite::TrackID keys;
        makeAndReopen (s, keys, false);
        juce::int64 time = 0;
        juce::StringArray reasons;

        for (const auto& snapshot : phazite::getSnapshots (s.context))
        {
            reasons.add (snapshot.reason);

            for (const auto& track : phazite::getTrackElements (phazite::getVersionHistory (s.context)->readSnapshot (snapshot.time)))
                if (phazite::readID (track[schema::trackID]) == keys.getRaw())
                    time = snapshot.time;
        }

        INFO (reasons.joinIntoString ("; ").toStdString());
        REQUIRE (time != 0);
        REQUIRE (phazite::restoreSnapshot (s.context, time));
        checkNextItems (s, keys);
    }

    // The last commit restored whole.
    {
        TestSong s;
        phazite::TrackID keys;
        makeAndReopen (s, keys, true);
        REQUIRE (phazite::bringBackVersion (s.context, phazite::getLastVersion (s.context)->id));
        checkNextItems (s, keys);
    }

    // One track of it: the song lacks Keys, which its last commit has, and its line is discarded.
    {
        TestSong s;
        phazite::TrackID keys;
        makeAndReopen (s, keys, true);
        const auto line = findLine (phazite::getListedChanges (s.context), "Keys");
        REQUIRE (line.has_value());
        REQUIRE (phazite::takeBack (s.context, *line));
        checkNextItems (s, keys);
    }
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

TEST_CASE ("A track brought back from a commit whose clip can't be read comes back without that clip, and no stand-in reaches the song")
{
    // Bass, with a clip, as a commit holds it; then the clip's piece lost, and the song joined as much as can be read, as
    // Restore from This Commit reads a damaged commit (SongPieces.h: joinSong()).
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::setTrackName (*song, tracks[0], "Bass");
    const auto clip = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
    addMidiNote (*song, clip, 36, at (0.0), lasting (1.0));

    auto pieces = phazite::splitSong (song->getState());
    juce::String lost;

    for (const auto& [name, block] : pieces.pieces)
        if (block.toString().startsWith ("<" + schema::CLIP.toString()))
            lost = name;

    REQUIRE (lost.isNotEmpty());
    pieces.pieces.erase (lost);
    std::vector<phazite::MissingPiece> missing;
    const auto then = phazite::joinSong (
        pieces.song,
        [&pieces] (const juce::String& name)
        {
            const auto found = pieces.pieces.find (name);
            return found != pieces.pieces.end() ? std::optional<juce::MemoryBlock> (found->second) : std::nullopt;
        },
        nullptr, &missing);

    REQUIRE (then.isValid());
    REQUIRE (missing.size() == 1);

    const std::function<bool (const juce::ValueTree&)> holdsStandIn = [&holdsStandIn] (const juce::ValueTree& tree)
    {
        if (tree.hasProperty (schema::versions::missing))
            return true;

        for (const auto& child : tree)
            if (holdsStandIn (child))
                return true;

        return false;
    };

    REQUIRE (holdsStandIn (then));

    // Bass deleted since.
    {
        const phazite::UndoStep step (*song, "Delete Track");
        phazite::deleteTracks (*song, { tracks[0] }, nullptr);
    }

    const auto check = [&]
    {
        const auto bass = phazite::findTrackState (*song, tracks[0]);
        REQUIRE (bass.isValid());
        CHECK (bass[schema::trackName].toString() == "Bass");
        CHECK (phazite::getClipStates (bass).empty()); // what the clip held stays as it is now: not there
        CHECK_FALSE (holdsStandIn (song->getState()));
        checkIDs (*song);
    };

    // The whole song brought back.
    {
        const phazite::UndoStep step (*song, "Restore");
        phazite::makeSongLike (*song, then);
        phazite::removeBrokenReferences (*song);
    }

    check();
    phazite::undo (*song);
    REQUIRE_FALSE (phazite::findTrackState (*song, tracks[0]).isValid());

    // Its line alone.
    {
        const phazite::UndoStep step (*song, "Restore Bass");
        REQUIRE (phazite::makeTrackLike (*song, tracks[0], then));
        phazite::removeBrokenReferences (*song);
    }

    check();
}
