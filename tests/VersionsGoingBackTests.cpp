// SPDX-License-Identifier: AGPL-3.0-or-later
// Going back to a version (docs/design/versions.md: Going back, step V2): Bring Back This Version, Put Back and Take from This
// Version, each one undo step that what plays follows, with the fewest edits, and what isn't counted left as it is.
#include "GoldenFiles.h"
#include "RenderHarness.h"
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
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

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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

// What a commit changed, once the history's thread has read it.
std::vector<phazite::VersionChange> readVersionChanges (phazite::ProjectContext& context, const juce::String& versionID)
{
    phazite::getVersionChanges (context, versionID);
    REQUIRE (waitForVersionHistory (context));
    return phazite::getVersionChanges (context, versionID);
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

// The song holds together as its file would open (SongCheck.h: copyAsOpened(), without the curves an undo keeps), by every
// rule, the clips of an alias group holding the same notes among them; and its own values are kept with its tree (values).
void checkWhole (const phazite::Song& song)
{
    for (const auto& problem : findSongProblems (*copyAsOpened (song)))
        FAIL_CHECK (problem);

    for (const auto& problem : findValueProblems (song))
        FAIL_CHECK (problem);
}

// The notes of a track's clips, a clip's after another's in the track's order (TestClips.h: describeClipNotes()).
juce::String describeTrackNotes (const phazite::Song& song, phazite::TrackID track)
{
    juce::StringArray clips;

    for (const auto clip : phazite::getClips (song, track))
        clips.add (describeClipNotes (song, clip));

    return clips.joinIntoString (" | ");
}

// The notes of each clip of an alias group, a line each (TestClips.h: describeClipNotes()); and how many different ones
// they hold: one for a group whose clips hold the same notes.
juce::String describeGroupNotes (const phazite::Song& song, const std::vector<phazite::ClipID>& group)
{
    juce::StringArray lines;

    for (const auto clip : group)
        lines.add ("clip " + juce::String ((juce::int64) clip.getRaw()) + ": " + describeClipNotes (song, clip));

    return lines.joinIntoString ("\n");
}

int countDifferentNotes (const phazite::Song& song, const std::vector<phazite::ClipID>& group)
{
    juce::StringArray different;

    for (const auto clip : group)
        different.addIfNotAlreadyThere (describeClipNotes (song, clip));

    return different.size();
}

// A song for the going-back edits under the undo check (UndoCheck.h): Bass and Keys, each with Phazite's synth; on Bass a
// clip of three notes, an alias of it on Keys from beat 8 and another on Bass from beat 16, so that a note edited through the
// alias on Keys changes a line of each track. Kept as "First". Then, unless only First is asked for: a note moved and one
// added through the alias and Keys made quieter, kept as "Second"; and since, a note transposed and one deleted through the
// alias, Bass quieter and a track added.
struct NotesAndAliases
{
    explicit NotesAndAliases (bool withSecond = true)
    {
        s.setInstrument (bass, phazite::devices::Synth::typeID);
        s.setInstrument (keys, phazite::devices::Synth::typeID);
        s.addNote (clip, 36, 0.0, 1.0);
        s.addNote (clip, 43, 1.0, 1.0);
        s.addNote (clip, 48, 2.0, 0.5);
        onKeys = s.copyClip (clip, 8.0, true);
        phazite::moveClip (s.context, onKeys, phazite::BeatPosition::fromBeats (8.0), keys);
        later = s.copyClip (clip, 16.0, true);
        REQUIRE (phazite::getLinkedClips (s.context, clip).size() == 3);
        REQUIRE (phazite::getClipTrack (song(), onKeys) == keys);
        first = keepVersion (s.context, "First");
        firstText = writeSong (song(), SongPart::undoable);
        firstBass = describeTrackNotes (song(), bass);
        firstKeys = describeTrackNotes (song(), keys);

        if (! withSecond)
            return;

        editNote ("Move Note", 0, [] (phazite::Song& edited, phazite::NoteRef note)
                  { phazite::moveNote (edited, note, phazite::BeatPosition::fromBeats (8.5), 38); });
        s.addNote (onKeys, 50, 11.0, 0.5);
        s.setVolume (keys, -6.0f);
        second = keepVersion (s.context, "Second");
        secondText = writeSong (song(), SongPart::undoable);
        secondBass = describeTrackNotes (song(), bass);
        secondKeys = describeTrackNotes (song(), keys);

        editNote ("Transpose", 1, [] (phazite::Song& edited, phazite::NoteRef note) { phazite::transposeNotes (edited, { note }, 2); });
        editNote ("Delete Note", 2, [] (phazite::Song& edited, phazite::NoteRef note) { phazite::deleteNotes (edited, { note }); });
        s.setVolume (bass, -3.0f);
        s.addTrack ("Strings");
        checkWhole (song());
    }

    phazite::Song& song() const { return *phazite::findSong (s.context); }

    // A note of the alias on Keys, by its place in the clip, edited through that alias as one undo step.
    void editNote (const juce::String& name, int index, const std::function<void (phazite::Song&, phazite::NoteRef)>& edit)
    {
        const auto notes = phazite::getClipNotes (song(), onKeys);
        REQUIRE ((int) notes.size() > index);
        const phazite::UndoStep step (song(), name);
        edit (song(), { onKeys, notes[(size_t) index].id });
    }

    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass"), keys = s.addTrack ("Keys");
    phazite::ClipID clip = s.addMidiClip (bass, 0.0, 4.0), onKeys, later;
    juce::String first, second;

    // Each version's song as an undo step keeps it (UndoCheck.h: SongPart::undoable), and the notes of Bass's and Keys's clips
    // in it, as it was saved.
    juce::String firstText, secondText, firstBass, firstKeys, secondBass, secondKeys;
};
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
    const auto then = phazite::test::copyWholeSong (song);

    // Changes of each kind, and some view state.
    s.setVolume (bass, -12.0f);
    s.setParameter (synth, "cutoff", 900.0f);
    s.addNote (clip, 40, 2.0, 1.0);
    s.addEffect (bass, phazite::devices::Equalizer::typeID);
    s.addTrack ("Strings");
    s.setTempo (128.0);
    s.rename (bass, "Low");
    phazite::findTrackState (song, bass).setProperty (schema::trackHeight, 140, nullptr);
    const auto changed = phazite::test::copyWholeSong (song);
    REQUIRE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::bringBackVersion (s.context, verse); }));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::findChanges (then, phazite::test::copyWholeSong (song)).empty());
    CHECK ((int) phazite::findTrackState (song, bass)[schema::trackHeight] == 140); // view state stays
    CHECK (phazite::describeUndo (&song) == "Undo Restore \"Verse\"");
    CHECK (phazite::getVersions (s.context).size() == 2); // the history keeps every commit, the initial one too

    // Undo takes it all back, in one step.
    phazite::undo (song);
    CHECK (phazite::findChanges (changed, phazite::test::copyWholeSong (song)).empty());
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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
    CHECK (describeChanges (phazite::getListedChanges (withChangesIn (s.context))) == "Bass: volume; + Strings; - Keys");

    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Bass")));
    CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-12.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard Changes in Bass");

    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Strings")));
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Keys")));
    CHECK (describeChanges (phazite::getListedChanges (withChangesIn (s.context))).isEmpty());

    // Keys came back where it was, after Bass, with its clip and its note.
    const auto tracks = phazite::getTrackIDs (*phazite::findSong (s.context));
    REQUIRE (tracks.size() == 3);
    CHECK (tracks[1] == bass);
    CHECK (tracks[2] == keys);
    CHECK (phazite::getClips (*phazite::findSong (s.context), keys).size() == 1);

    // Take from This Version: what differs from the first version, and Bass taken from it.
    phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, first); });
    CHECK (phazite::getComparedVersion (s.context) == first);
    CHECK (describeChanges (phazite::getListedChanges (withChangesIn (s.context))) == "Bass: volume");
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Bass")));
    CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Restore Bass from \"First\"");
    CHECK (describeChanges (phazite::getListedChanges (withChangesIn (s.context))).isEmpty());

    // Back to the changes since the last version, which that one is now.
    phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, {}); });
    CHECK (describeChanges (phazite::getListedChanges (withChangesIn (s.context))) == "Bass: volume");
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
    const auto changes = [&s] { return describeChanges (phazite::getListedChanges (withChangesIn (s.context))); };

    // Grouped into a folder since: the folder's line discarded, its tracks are where they were, with what they hold.
    phazite::TrackID folder;

    {
        const phazite::UndoStep step (song, "Group");
        folder = phazite::groupIntoFolder (song, { bass, keys }, nullptr);
    }

    REQUIRE (findLine (phazite::getListedChanges (withChangesIn (s.context)), "Folder").has_value());
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Folder")));
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

    REQUIRE (findLine (phazite::getListedChanges (withChangesIn (s.context)), "Folder").has_value());
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Folder")));
    checkIDs (song);
    CHECK (phazite::getTrackIDs (song).size() == 4);
    CHECK (phazite::getTracksInFolder (song, folder).empty());
    REQUIRE (findLine (phazite::getListedChanges (withChangesIn (s.context)), "Song").has_value());
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Song")));
    checkIDs (song);
    CHECK (phazite::getTracksInFolder (song, folder) == std::vector { bass, keys });
    CHECK (changes().isEmpty());

    // Deleted since with its tracks: the folder's line discarded, it is back with them, each once.
    {
        const phazite::UndoStep step (song, "Delete");
        phazite::deleteTracks (song, { folder }, nullptr);
    }

    REQUIRE (phazite::getTrackIDs (song).size() == 1);
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Folder")));
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
    const auto changes = [&s] { return describeChanges (phazite::getListedChanges (withChangesIn (s.context))); };

    const auto moveToKeys = [&song, clip, keys]
    {
        const phazite::UndoStep step (song, "Move Clip");
        phazite::moveClip (song, clip, phazite::BeatPosition::fromBeats (2.0), keys);
    };

    // Moved onto Keys since. Discard Changes in Bass: the clip is on Bass again, and on Keys no more.
    moveToKeys();
    CHECK (changes() == "Bass: 1 clip removed; Keys: 1 clip added");
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Bass")));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    CHECK (changes().isEmpty());

    // One undo step: on Keys again, once.
    phazite::undo (song);
    checkIDs (song);
    CHECK (phazite::getClips (song, bass).empty());
    CHECK (phazite::getClips (song, keys) == std::vector { clip });

    // Committed on Keys, then Bass restored from the commit before: the clip is on Bass, once.
    keepVersion (s.context, "Moved");
    const auto bassLine = findLine (readVersionChanges (s.context, phazite::getLastVersion (s.context)->id), "Bass");
    REQUIRE (bassLine.has_value());
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, first, *bassLine); }));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    phazite::undo (song);

    // Bass deleted since, after its clip went onto Keys (the last commit has both so): Bass back, with its clip, once.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::bringBackVersion (s.context, first); }));
    keepVersion (s.context, "As at first");
    moveToKeys();

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { bass }, nullptr);
    }

    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Bass")));
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

    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
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
    const auto changes = [&s] { return describeChanges (phazite::getListedChanges (withChangesIn (s.context))); };

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
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Keys")));
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
    const auto keysLine = findLine (readVersionChanges (s.context, phazite::getLastVersion (s.context)->id), "Keys");
    REQUIRE (keysLine.has_value());
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::restoreFromVersion (s.context, first, *keysLine); }));
    checkIDs (song);
    CHECK (phazite::getClips (song, bass) == std::vector { clip });
    CHECK (phazite::getClips (song, keys).empty());
    phazite::undo (song);

    // Moved onto a track added since: that track's line discarded, the track goes, and the clip is on Keys again.
    const auto strings = s.addTrack ("Strings");
    moveTo (strings);
    REQUIRE (phazite::takeBack (s.context, *findLine (phazite::getListedChanges (withChangesIn (s.context)), "Strings")));
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
        const auto saved = phazite::test::copyWholeSong (*phazite::findSong (s.context));

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
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::restoreSnapshot (s.context, time); }));
        checkNextItems (s, keys);
    }

    // The last commit restored whole.
    {
        TestSong s;
        phazite::TrackID keys;
        makeAndReopen (s, keys, true);
        REQUIRE (phazite::test::finished (s.context,
                                          [&] { return phazite::bringBackVersion (s.context, phazite::getLastVersion (s.context)->id); }));
        checkNextItems (s, keys);
    }

    // The song grew from the version before Keys (its names say so), so the changes don't list Keys as taken away: the
    // branch's newer version is offered instead, and taken.
    {
        TestSong s;
        phazite::TrackID keys;
        makeAndReopen (s, keys, true);
        CHECK_FALSE (findLine (phazite::getListedChanges (withChangesIn (s.context)), "Keys").has_value());
        REQUIRE (phazite::countNewerVersions (s.context) == 1);
        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::takeNewerVersions (s.context); }));
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
    CHECK (phazite::getCurrentName (*commands.find ("version.bringBack")) == "Restore \"First\"");
    CHECK_FALSE (phazite::canTakeFrom (s.context)); // it is the last version: its changes are listed already
    REQUIRE (phazite::test::finished (s.context, [&] { return commands.invoke ("version.bringBack"); }));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    s.setVolume (bass, -9.0f);
    phazite::selectChange (s.context, phazite::getListedChanges (withChangesIn (s.context)).front());
    REQUIRE (phazite::test::finished (s.context, [&] { return commands.invoke ("version.putBack"); }));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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

    // A moment later, as a person commits: the plug-in, opened from no state of the song's, has given its own (a commit
    // writes what each gave last: model/plugins/internal/PluginInstances.h).
    REQUIRE (dispatchMessagesUntil (
        [&s]
        {
            const auto& song = *phazite::findSong (s.context);
            return phazite::getPluginsWithOlderStates (song).isEmpty() && phazite::getPluginsAnswering (song).isEmpty();
        }));
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
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::bringBackVersion (s.context, version); }));
    CHECK (writes.count == 0);

    // The plug-in's own settings changed in its window: they are set back to the version's, and Undo puts them back.
    TestPlugin::setGain (song, device, gain * 0.25f);
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::bringBackVersion (s.context, version); }));
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
        REQUIRE (waitForVersionHistory (context));
        const auto version = phazite::getLastVersion (context)->id; // its first version, the song as it came
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
                    if (phazite::isMidiClip (song, clips.front()))
                        phazite::addNote (song, clips.front(), 64, phazite::BeatPosition::fromBeats (0.5),
                                          phazite::BeatDuration::fromBeats (0.25), 90);

                    phazite::deleteClips (song, { clips.back() }, false);
                    break;
                }
            }

            phazite::deleteTracks (song, { tracks.back() }, nullptr);
        }

        REQUIRE (phazite::hasChangesSinceLastVersion (withChangesIn (context)));
        REQUIRE (phazite::test::finished (context, [&] { return phazite::bringBackVersion (context, version); }));
        INFO (describeChanges (phazite::getChangesSinceLastVersion (withChangesIn (context))).toStdString());
        CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (context)));

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

    auto pieces = phazite::splitSong (phazite::test::copyWholeSong (*song));
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
        CHECK (phazite::getClips (*song, tracks[0]).empty());              // what the clip held stays as it is now: not there
        CHECK_FALSE (holdsStandIn (phazite::test::copyWholeSong (*song))); // whole: its clips too
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

TEST_CASE ("Every way of going back is one exact undo step on a song of notes and aliases, which holds together after it, its Undo and "
           "its Redo")
{
    NotesAndAliases n;
    auto& s = n.s;
    auto& song = n.song();
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, s.context, [] (std::function<void (bool)> then) { then (true); }, s.renders);
    UndoCheck undo (s.context);

    const auto lineOf = [&s] (const juce::String& name)
    {
        const auto line = findLine (phazite::getListedChanges (withChangesIn (s.context)), name);
        INFO ("no line " << name.toStdString() << " among " << describeChanges (phazite::getListedChanges (s.context)).toStdString());
        REQUIRE (line.has_value());
        return *line;
    };

    // What a going-back edit must leave: the song as an undo step keeps it, the same as a text kept before; or a track's
    // clips holding the notes a version held.
    const auto songIs = [&song] (const juce::String& expected)
    {
        return [&song, expected]
        {
            const auto differences = describeSongDifferences (expected, writeSong (song, SongPart::undoable));
            INFO (differences.joinIntoString ("\n").toStdString());
            CHECK (differences.isEmpty());
        };
    };

    const auto notesAre = [&song] (phazite::TrackID track, const juce::String& expected)
    { return [&song, track, expected] { CHECK (describeTrackNotes (song, track) == expected); }; };

    // Each through the undo check, what it must give checked inside it, the song checked after it, after its Undo and after
    // its Redo; then undone again, so that the next starts from the changes since Second. An edit of one track's line alone
    // gives the clips of an alias group on other tracks the notes it restores (known bug 234, fixed: the cases after this one).
    const auto goBack = [&] (const juce::String& what, const std::function<bool()>& command, const std::function<void()>& gives)
    {
        INFO (what.toStdString());
        undo.check (what,
                    [&]
                    {
                        REQUIRE (phazite::test::finished (s.context, command));
                        gives();
                    });
        checkWhole (song);
        undo.stepBackOrForth ("Undo " + what, [&song] { phazite::undo (song); });
        checkWhole (song);
        undo.stepBackOrForth ("Redo " + what, [&song] { phazite::redo (song); });
        checkWhole (song);
        undo.stepBackOrForth ("Undo " + what + " again", [&song] { phazite::undo (song); });
    };

    // Put Back, a line each, against the last version (Second): Keys (the alias that the notes were edited through), then
    // Bass (the group's first and last).
    const auto keysLine = lineOf ("Keys");
    goBack ("Put Back Keys", [&] { return phazite::takeBack (s.context, keysLine); }, notesAre (n.keys, n.secondKeys));
    const auto bassLine = lineOf ("Bass");
    goBack ("Put Back Bass", [&] { return phazite::takeBack (s.context, bassLine); }, notesAre (n.bass, n.secondBass));

    // Discard All Changes, and Restore This Version on the last version and on the one before, by the command.
    goBack ("Discard All Changes", [&] { return phazite::discardAllChanges (s.context); }, songIs (n.secondText));
    goBack ("Restore Second", [&] { return phazite::bringBackVersion (s.context, n.second); }, songIs (n.secondText));
    phazite::selectVersion (s.context, n.first);
    REQUIRE (phazite::canBringBack (s.context));
    goBack ("version.bringBack", [&] { return commands.invoke ("version.bringBack"); }, songIs (n.firstText));

    // Take from This Version: a line of what differs from First, taken from it.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, n.first); }));
    const auto keysFromFirst = lineOf ("Keys");
    goBack ("Take Keys from First", [&] { return phazite::takeBack (s.context, keysFromFirst); }, notesAre (n.keys, n.firstKeys));
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, {}); }));

    // A line of what Second changed, restored from First, and one from Second.
    const auto secondsLines = readVersionChanges (s.context, n.second);
    const auto keysOfSecond = findLine (secondsLines, "Keys"), bassOfSecond = findLine (secondsLines, "Bass");
    INFO ("Second's lines: " << describeChanges (secondsLines).toStdString());
    REQUIRE (keysOfSecond.has_value());
    REQUIRE (bassOfSecond.has_value());
    goBack (
        "Restore Keys from First", [&] { return phazite::restoreFromVersion (s.context, n.first, *keysOfSecond); },
        notesAre (n.keys, n.firstKeys));
    goBack (
        "Restore Bass from Second", [&] { return phazite::restoreFromVersion (s.context, n.second, *bassOfSecond); },
        notesAre (n.bass, n.secondBass));

    // An automatic save restored: taken of the song as it is, then a note's velocity set through the alias, and the save
    // brought back, which gives the song as it was before the velocity (not merely a step that Undo takes back).
    REQUIRE (waitForVersionHistory (s.context));
    phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE_FALSE (phazite::getSnapshots (s.context).empty());
    const auto saved = phazite::getSnapshots (s.context).front().time; // the newest
    const auto beforeVelocity = writeSong (song, SongPart::undoable);
    undo.check ("a velocity",
                [&n]
                {
                    n.editNote ("Set Velocity", 0,
                                [] (phazite::Song& edited, phazite::NoteRef note) { phazite::setNoteVelocities (edited, { note }, 70); });
                });
    goBack ("Restore the automatic save", [&] { return phazite::restoreSnapshot (s.context, saved); }, songIs (beforeVelocity));

    // A version opened live and closed: the song is the version's while it is open, and Now's again, whole, after Back to Now.
    const auto now = writeSong (song, SongPart::whole);
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::openVersion (s.context, n.first, s.save); }));
    REQUIRE (phazite::getOpenVersion (s.context) == n.first);
    REQUIRE (waitForVersionHistory (s.context));
    checkWhole (song);
    CHECK (describeSongDifferences (n.firstText, writeSong (song, SongPart::undoable)).isEmpty());
    REQUIRE (phazite::backToNow (s.context));
    CHECK (phazite::getOpenVersion (s.context).isEmpty());
    CHECK (describeSongDifferences (now, writeSong (song, SongPart::whole)).isEmpty());
    checkWhole (song);

    // Every step back to where the check began, and forth again.
    undo.walkBackAndForth();
    checkWhole (song);
}

// What the audio clips of an alias group share (AudioClips.h: shareTheSound(): the file, tempo, pitch, direction and warp
// markers) is given by one track's line put back, taken from a version or restored to every clip of the group, those on other
// tracks too, in the same undo step (known bug 234, its audio half): a group of three clips on Drums and Loops, the shared
// pitch changed and a warp marker added through the clip on Loops, kept as Second; the pitch changed again since; then
// Drums's line taken back each way, and once more with Drums deleted since, each through the undo check.
TEST_CASE ("Every way of going back a track's line gives the audio clips of an alias group on other tracks what the group shares, "
           "as one exact undo step")
{
    TestSong s;
    const auto drums = s.addTrack ("Drums"), loops = s.addTrack ("Loops");
    const auto file = writeSound (s.getSoundsFolder().getChildFile ("Tone.wav"), 2.0);
    const auto clip = s.addAudioClip (drums, file, 0.0);
    const auto onLoops = s.copyClip (clip, 8.0, true);
    phazite::moveClip (s.context, onLoops, phazite::BeatPosition::fromBeats (8.0), loops);
    const auto later = s.copyClip (clip, 16.0, true);
    std::vector members { clip, onLoops, later };
    auto& song = *phazite::findSong (s.context);
    REQUIRE (phazite::getLinkedClips (s.context, clip).size() == 3);
    REQUIRE (phazite::getClipTrack (song, onLoops) == loops);
    const auto first = keepVersion (s.context, "First");

    phazite::setClipPitch (s.context, onLoops, 3.5f);
    REQUIRE (phazite::addWarpMarker (s.context, onLoops, 0.5));
    const auto second = keepVersion (s.context, "Second");
    phazite::setClipPitch (s.context, onLoops, -2.0f);

    // Every clip of the group shares the sound of the first, at a pitch, with so many warp markers.
    const auto shareAt = [&song, &members] (float pitch, std::size_t markers)
    {
        return [&song, &members, pitch, markers]
        {
            const auto firstState = phazite::copyWholeClip (song, members.front()); // each whole, as the loader compares them

            for (const auto member : members)
            {
                INFO ("clip " << member.getRaw());
                CHECK (phazite::shareTheSound (firstState, phazite::copyWholeClip (song, member)));
                CHECK (phazite::getClipPitch (song, member) == doctest::Approx (pitch));
                CHECK (phazite::getWarpMarkers (song, member).size() == markers);
            }
        };
    };

    UndoCheck undo (s.context);
    const auto lineOf = [&s] (const juce::String& name)
    {
        const auto line = findLine (phazite::getListedChanges (withChangesIn (s.context)), name);
        INFO ("no line " << name.toStdString() << " among " << describeChanges (phazite::getListedChanges (s.context)).toStdString());
        REQUIRE (line.has_value());
        return *line;
    };

    // As the case above does: through the undo check, the song checked (the alias rule among the rest) after it, its Undo and
    // its Redo, then undone again.
    const auto goBack = [&] (const juce::String& what, const std::function<bool()>& command, const std::function<void()>& gives)
    {
        INFO (what.toStdString());
        undo.check (what,
                    [&]
                    {
                        REQUIRE (phazite::test::finished (s.context, command));
                        gives();
                    });
        checkWhole (song);
        undo.stepBackOrForth ("Undo " + what, [&song] { phazite::undo (song); });
        checkWhole (song);
        undo.stepBackOrForth ("Redo " + what, [&song] { phazite::redo (song); });
        checkWhole (song);
        undo.stepBackOrForth ("Undo " + what + " again", [&song] { phazite::undo (song); });
    };

    const auto drumsLine = lineOf ("Drums");
    goBack ("Put Back Drums", [&] { return phazite::takeBack (s.context, drumsLine); }, shareAt (3.5f, 1u));

    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, first); }));
    const auto drumsFromFirst = lineOf ("Drums");
    goBack ("Take Drums from First", [&] { return phazite::takeBack (s.context, drumsFromFirst); }, shareAt (0.0f, 0u));
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, {}); }));

    const auto drumsOfSecond = findLine (readVersionChanges (s.context, second), "Drums");
    REQUIRE (drumsOfSecond.has_value());
    goBack ("Restore Drums from First", [&] { return phazite::restoreFromVersion (s.context, first, *drumsOfSecond); }, shareAt (0.0f, 0u));

    // A second alias on Loops, so that the group outlives Drums (its last clip would leave it); Drums deleted since, the
    // group's clips left on Loops given another pitch, and Drums's line put back: the track comes back with its clips of the
    // group, whose sound the clips on Loops take.
    undo.checkSeveral ("Alias on Loops", [&] { members.push_back (s.copyClip (onLoops, 24.0, true)); });
    REQUIRE (phazite::getLinkedClips (s.context, clip).size() == 4);
    undo.check ("Delete Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Delete Track");
                    phazite::deleteTracks (song, { drums }, nullptr);
                });
    undo.check ("Change Clip Pitch", [&] { phazite::setClipPitch (s.context, onLoops, 5.0f); });
    REQUIRE (phazite::getLinkedClips (s.context, onLoops).size() == 2);
    const auto drumsGone = lineOf ("Drums");
    goBack ("Put Back Drums, deleted since", [&] { return phazite::takeBack (s.context, drumsGone); }, shareAt (3.5f, 1u));

    undo.walkBackAndForth();
    checkWhole (song);
}

// Put Back of one track's line (takeBack(); Take from This Version is the same) makes that track's clips of an alias group what
// they were, as one value of the group (Notes.h: makeClipNotesLike()), so that the group's clips on other tracks take the
// restored notes too, as a merge gives every clip of a group its keeper's notes (docs/design/versions.md): known bug 234,
// which left them as they were.
TEST_CASE ("Put Back of one track's line keeps the clips of an alias group on other tracks holding the same notes")
{
    NotesAndAliases n;
    const auto keys = findLine (phazite::getListedChanges (withChangesIn (n.s.context)), "Keys");
    REQUIRE (keys.has_value());
    REQUIRE (phazite::test::finished (n.s.context, [&] { return phazite::takeBack (n.s.context, *keys); }));
    REQUIRE (describeTrackNotes (n.song(), n.keys) == n.secondKeys);

    // The group's clips, Keys's among them, hold the same notes.
    const auto group = phazite::getLinkedClips (n.s.context, n.clip);
    REQUIRE (group.size() == 3);
    INFO (describeGroupNotes (n.song(), group).toStdString());
    CHECK (countDifferentNotes (n.song(), group) == 1);
}

// A line of a version restored alone (restoreFromVersion(), version.restoreChange) does the same: the track's clips of an alias
// group become the version's, and so do the group's clips on other tracks (known bug 234).
TEST_CASE ("A track's line restored from a version keeps the clips of an alias group on other tracks holding the same notes")
{
    NotesAndAliases n;
    const auto keys = findLine (readVersionChanges (n.s.context, n.second), "Keys");
    REQUIRE (keys.has_value());
    REQUIRE (phazite::test::finished (n.s.context, [&] { return phazite::restoreFromVersion (n.s.context, n.first, *keys); }));
    REQUIRE (describeTrackNotes (n.song(), n.keys) == n.firstKeys);

    // The group's clips, Keys's among them, hold the same notes.
    const auto group = phazite::getLinkedClips (n.s.context, n.clip);
    REQUIRE (group.size() == 3);
    INFO (describeGroupNotes (n.song(), group).toStdString());
    CHECK (countDifferentNotes (n.song(), group) == 1);
}

TEST_CASE ("A merge of note edits made through an alias on both branches is one exact undo step, the group whole after it, its Undo "
           "and its Redo")
{
    NotesAndAliases n (false);
    auto& s = n.s;
    auto& song = n.song();

    // idea-2: the first note up two semitones through the alias on Keys. main: the last note's velocity, through it too.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    n.editNote ("Transpose", 0, [] (phazite::Song& edited, phazite::NoteRef note) { phazite::transposeNotes (edited, { note }, 2); });
    const auto theirs = keepVersion (s.context, "Up");
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    n.editNote ("Set Velocity", 2,
                [] (phazite::Song& edited, phazite::NoteRef note) { phazite::setNoteVelocities (edited, { note }, 70); });
    keepVersion (s.context, "Softer");
    const auto mainNotes = phazite::getClipNotes (song, n.clip);
    REQUIRE (mainNotes.size() == 3);

    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
    UndoCheck undo (s.context);
    undo.check ("Merge", [&s] { REQUIRE (phazite::finishMerge (s.context)); });
    REQUIRE (phazite::isMerging (s.context));
    CHECK (phazite::getClashes (s.context).empty());
    checkWhole (song);

    // Both edits in every clip of the group.
    for (const auto member : { n.clip, n.onKeys, n.later })
    {
        INFO ("clip " << member.getRaw());
        const auto notes = phazite::getClipNotes (song, member);
        REQUIRE (notes.size() == 3);
        CHECK (notes[0].pitch == mainNotes[0].pitch + 2);
        CHECK (notes[2].velocity == 70);
    }

    undo.stepBackOrForth ("Undo Merge", [&song] { phazite::undo (song); });
    CHECK_FALSE (phazite::isMerging (s.context));
    checkWhole (song);
    undo.stepBackOrForth ("Redo Merge", [&song] { phazite::redo (song); });
    CHECK (phazite::isMerging (s.context));
    checkWhole (song);
    undo.walkBackAndForth();
    checkWhole (song);
}

//==============================================================================
// The clips made like a version's (ApplySong.cpp: ClipPass), by the clips' functions on their IDs and values: a clip moved
// between tracks keeps its ID, whichever of the two tracks comes first; a track's clips take the version's order; an alias
// group's notes and sound are given as one, to its clips on other tracks too; and what the version couldn't read stays as it
// is, where it is.
namespace
{
// A clip's element in a song's tree (a version's), invalid for one it hasn't.
juce::ValueTree findClipElement (const juce::ValueTree& song, phazite::ClipID clip)
{
    for (const auto& track : phazite::getTrackElements (song))
        for (const auto& child : track)
            if (child.hasType (schema::CLIP) && phazite::readID (child[schema::clipID]) == clip.getRaw())
                return child;

    return {};
}

// Whether the song is the version, whole: no change between them (SongDiff.h: findChanges()); each track's clips in the
// version's order, with their IDs; each clip as the version holds it, its properties, markers and notes as a piece writes
// them; and the song holding together.
void checkSongIs (const phazite::Song& song, const juce::ValueTree& version)
{
    const auto changes = phazite::findChanges (version, phazite::test::copyWholeSong (song));
    INFO (phazite::summarise (changes).toStdString());
    CHECK (changes.empty());

    for (const auto& track : phazite::getTrackElements (version))
    {
        std::vector<phazite::ClipID> clips;

        for (const auto& child : track)
            if (child.hasType (schema::CLIP))
                clips.push_back (phazite::ClipID::fromRaw (phazite::readID (child[schema::clipID])));

        const auto id = phazite::TrackID::fromRaw (phazite::readID (track[schema::trackID]));
        CHECK (phazite::getClips (song, id) == clips);

        for (const auto clip : clips)
        {
            INFO ("clip " << clip.getRaw());
            CHECK (phazite::writeElement (phazite::copyWholeClip (song, clip)).toString()
                   == phazite::writeElement (findClipElement (version, clip)).toString());
        }
    }

    checkWhole (song);
    checkIDs (song);
}

// The song made like a version as Bring Back makes it, one undo step through the undo check: the version's after it and
// after its Redo, and whole after its Undo.
void bringBackChecked (phazite::Song& song, UndoCheck& undo, const juce::ValueTree& version, const juce::String& what)
{
    INFO (what.toStdString());
    undo.check (what,
                [&]
                {
                    const phazite::UndoStep step (song, what);
                    phazite::makeSongLike (song, version);
                    phazite::removeBrokenReferences (song);
                });
    checkSongIs (song, version);
    undo.stepBackOrForth ("Undo " + what, [&song] { phazite::undo (song); });
    checkWhole (song);
    undo.stepBackOrForth ("Redo " + what, [&song] { phazite::redo (song); });
    checkSongIs (song, version);
}

// One track's line taken from a version as Put Back takes it, one undo step through the undo check, the song whole after it
// and its Undo, which is then left done.
void takeLineChecked (phazite::Song& song, UndoCheck& undo, phazite::TrackID track, const juce::ValueTree& version,
                      const juce::String& what, const std::function<void()>& gives)
{
    INFO (what.toStdString());
    undo.check (what,
                [&]
                {
                    const phazite::UndoStep step (song, what);
                    REQUIRE (phazite::makeTrackLike (song, track, version));
                    phazite::removeBrokenReferences (song);
                    gives();
                });
    checkWhole (song);
    checkIDs (song);
    undo.stepBackOrForth ("Undo " + what, [&song] { phazite::undo (song); });
    checkWhole (song);
}

// What a song's tree is told of while a version is made the song's: each property set (by its element's type and the
// property's name), each element added or taken away, each moved among its parent's.
struct TreeChanges final : juce::ValueTree::Listener
{
    explicit TreeChanges (phazite::Song& s)
        : state (s.getState())
    {
        state.addListener (this);
    }

    ~TreeChanges() override { state.removeListener (this); }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        properties.add (tree.getType().toString() + "@" + property.toString());
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++added; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++removed; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { ++moved; }

    void reset()
    {
        properties.clear();
        added = removed = moved = 0;
    }

    juce::ValueTree state;
    juce::StringArray properties;
    int added = 0, removed = 0, moved = 0;
};
} // namespace

TEST_CASE ("A clip moved to another track since a version goes back and forth with its ID, whichever of the two tracks comes first")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto first = tracks[0], second = tracks[1];
    phazite::ClipID moved, onFirst, onSecond;

    {
        const phazite::UndoStep step (*song, "Clips");
        moved = addMidiClip (*song, first, { at (0.0), at (4.0) }, "Moved");
        addMidiNote (*song, moved, 60, at (0.0), lasting (1.0));
        onFirst = addMidiClip (*song, first, { at (8.0), at (12.0) }, "First");
        onSecond = addMidiClip (*song, second, { at (0.0), at (4.0) }, "Second");
        addMidiNote (*song, onSecond, 64, at (1.0), lasting (1.0));
    }

    const auto atFirst = phazite::test::copyWholeSong (*song);

    {
        const phazite::UndoStep step (*song, "Move Clip");
        phazite::moveClip (*song, moved, at (4.0), second);
    }

    const auto atSecond = phazite::test::copyWholeSong (*song);
    phazite::moveTracksAfter (*song, { second }, {});
    const auto atSecondOnTop = phazite::test::copyWholeSong (*song);
    REQUIRE (phazite::getTrackIDs (*song).front() == second);

    UndoCheck undo (*song);

    // The track it goes to comes first in the version: back onto the first track; then the track it leaves comes first:
    // onto the second track again; and with the tracks in another order in the song and the version, each way.
    bringBackChecked (*song, undo, atFirst, "Back onto the first track, after the tracks swapped places");
    CHECK (phazite::getClips (*song, first) == std::vector { moved, onFirst });
    CHECK (phazite::getClips (*song, second) == std::vector { onSecond });
    bringBackChecked (*song, undo, atSecond, "Onto the second track, the first coming first");
    CHECK (phazite::getClips (*song, first) == std::vector { onFirst });
    CHECK (phazite::getClips (*song, second) == std::vector { onSecond, moved });
    bringBackChecked (*song, undo, atFirst, "Back onto the first track, the first coming first");
    bringBackChecked (*song, undo, atSecondOnTop, "Onto the second track, the second coming first");
    CHECK (phazite::getTrackIDs (*song).front() == second);
    CHECK (phazite::getClips (*song, second) == std::vector { onSecond, moved });

    // A line alone: the first track's brings the clip back from the second, where it is now, into the version's order; the
    // second's sends it back to the first, as it is now, before the first's other clip, which comes after it in the version.
    takeLineChecked (*song, undo, first, atFirst, "The first track's line",
                     [&]
                     {
                         CHECK (phazite::getClips (*song, first) == std::vector { moved, onFirst });
                         CHECK (phazite::getClips (*song, second) == std::vector { onSecond });
                     });
    takeLineChecked (*song, undo, second, atFirst, "The second track's line",
                     [&]
                     {
                         CHECK (phazite::getClips (*song, first) == std::vector { moved, onFirst });
                         CHECK (phazite::getClips (*song, second) == std::vector { onSecond });
                     });

    undo.walkBackAndForth();

    // Another branch's song, outside the history, each way.
    phazite::makeSongTheSame (*song, atFirst);
    checkSongIs (*song, atFirst);
    phazite::makeSongTheSame (*song, atSecond);
    checkSongIs (*song, atSecond);
}

TEST_CASE ("A version in which two tracks swapped a clip each gives each its clip back, both with their IDs, in one undo step")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::ClipID one, two;

    {
        const phazite::UndoStep step (*song, "Clips");
        one = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "One");
        addMidiNote (*song, one, 60, at (0.0), lasting (1.0));
        two = addMidiClip (*song, tracks[1], { at (4.0), at (8.0) }, "Two");
        addMidiNote (*song, two, 67, at (1.0), lasting (2.0));
    }

    const auto before = phazite::test::copyWholeSong (*song);

    {
        const phazite::UndoStep step (*song, "Swap Clips");
        phazite::moveClip (*song, one, at (0.0), tracks[1]);
        phazite::moveClip (*song, two, at (4.0), tracks[0]);
    }

    const auto swapped = phazite::test::copyWholeSong (*song);
    UndoCheck undo (*song);
    bringBackChecked (*song, undo, before, "Swapped back");
    CHECK (phazite::getClips (*song, tracks[0]) == std::vector { one });
    CHECK (phazite::getClips (*song, tracks[1]) == std::vector { two });
    bringBackChecked (*song, undo, swapped, "Swapped again");
    CHECK (phazite::getClips (*song, tracks[0]) == std::vector { two });
    CHECK (phazite::getClips (*song, tracks[1]) == std::vector { one });

    // The first track's line alone: its clip back from the second, and the clip it holds now sent to the second.
    takeLineChecked (*song, undo, tracks[0], before, "The first track's line",
                     [&]
                     {
                         CHECK (phazite::getClips (*song, tracks[0]) == std::vector { one });
                         CHECK (phazite::getClips (*song, tracks[1]) == std::vector { two });
                     });

    undo.walkBackAndForth();
}

TEST_CASE ("A clip moved onto a track the version hasn't, from one it has that the song hasn't, comes back with its ID, and goes "
           "with the track")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto gone = tracks[0];
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*song, "Clip");
        clip = addMidiClip (*song, gone, { at (0.0), at (4.0) }, "Riff");
        addMidiNote (*song, clip, 48, at (0.0), lasting (1.0));
        addMidiNote (*song, clip, 55, at (1.0), lasting (1.0));
    }

    const auto before = phazite::test::copyWholeSong (*song);
    const auto added = phazite::addTrackAtEnd (*song, false, nullptr);

    {
        const phazite::UndoStep step (*song, "Move Clip");
        phazite::moveClip (*song, clip, at (0.0), added);
    }

    {
        const phazite::UndoStep step (*song, "Delete Track");
        phazite::deleteTracks (*song, { gone }, nullptr);
    }

    const auto after = phazite::test::copyWholeSong (*song);
    UndoCheck undo (*song);

    // The whole song: the track it was on back, with it; the track it is on gone, and back, with it.
    bringBackChecked (*song, undo, before, "Before");
    CHECK (phazite::getClips (*song, gone) == std::vector { clip });
    CHECK_FALSE (phazite::findTrackState (*song, added).isValid());
    bringBackChecked (*song, undo, after, "After");
    CHECK (phazite::getClips (*song, added) == std::vector { clip });
    CHECK_FALSE (phazite::findTrackState (*song, gone).isValid());

    // The gone track's line: it comes back, its clip from the track it is on now. The added track's line: it goes, and the
    // clip with it, as the song hasn't the track the version has it on.
    takeLineChecked (*song, undo, gone, before, "The gone track's line",
                     [&]
                     {
                         CHECK (phazite::getClips (*song, gone) == std::vector { clip });
                         CHECK (phazite::getClips (*song, added).empty());
                     });
    takeLineChecked (*song, undo, added, before, "The added track's line",
                     [&]
                     {
                         CHECK_FALSE (phazite::findTrackState (*song, added).isValid());
                         CHECK_FALSE (phazite::getClipValue (*song, clip).has_value());
                     });

    undo.walkBackAndForth();
}

TEST_CASE ("A version that holds a track's clips in another order moves them alone, and one that changes a clip's property sets "
           "that property alone")
{
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1)[0];
    std::vector<phazite::ClipID> clips;

    {
        const phazite::UndoStep step (*song, "Clips");

        for (const auto* name : { "P", "Q", "R" })
        {
            clips.push_back (
                addMidiClip (*song, track, { at (4.0 * (double) clips.size()), at (4.0 * (double) clips.size() + 4.0) }, name));
            addMidiNote (*song, clips.back(), 60 + (int) clips.size(), at (0.0), lasting (1.0));
        }
    }

    const auto before = phazite::test::copyWholeSong (*song);

    // The same clips, the other way round (as the version's file could hold them).
    auto reversed = before.createCopy();
    auto trackThere = phazite::getTrackElements (reversed).front();
    REQUIRE (phazite::readID (trackThere[schema::trackID]) == track.getRaw());

    for (const auto clip : clips)
        trackThere.moveChild (trackThere.indexOf (findClipElement (reversed, clip)), 0, nullptr);

    // Q renamed.
    auto renamed = before.createCopy();
    findClipElement (renamed, clips[1]).setProperty (schema::clipName, "Q2", nullptr);

    // Each made the song's as one undo step, what the song's tree is told of it and what the clips' listener is told kept, its
    // Undo and Redo exact.
    TreeChanges heard (*song);

    struct ClipsTold final : phazite::SongValues::ClipsListener
    {
        std::vector<phazite::ClipID> changed;
        std::vector<phazite::TrackID> reordered;

        void clipsChanged (const std::vector<phazite::SongValues::ClipChange>& clipsNow,
                           const std::vector<phazite::TrackID>& tracksNow) override
        {
            for (const auto& change : clipsNow)
            {
                CHECK (change.before == change.after); // in place, on its track
                changed.push_back (change.id);
            }

            reordered.insert (reordered.end(), tracksNow.begin(), tracksNow.end());
        }
    } told;

    song->getValues()->addClipsListener (&told);

    const auto apply = [&] (const juce::ValueTree& version, const juce::String& what)
    {
        INFO (what.toStdString());
        const auto textBefore = writeSong (*song, SongPart::undoable);
        heard.reset();
        told.changed.clear();
        told.reordered.clear();

        {
            const phazite::UndoStep step (*song, what);
            phazite::makeSongLike (*song, version);
        }

        const auto treeTold = heard.properties;
        const auto added = heard.added, removed = heard.removed, moved = heard.moved;
        const auto clipsTold = told;
        checkSongIs (*song, version);
        const auto textAfter = writeSong (*song, SongPart::undoable);
        phazite::undo (*song);
        CHECK (describeSongDifferences (textBefore, writeSong (*song, SongPart::undoable)).isEmpty());
        phazite::redo (*song);
        CHECK (describeSongDifferences (textAfter, writeSong (*song, SongPart::undoable)).isEmpty());
        heard.properties = treeTold;
        heard.added = added;
        heard.removed = removed;
        heard.moved = moved;
        told.changed = clipsTold.changed;
        told.reordered = clipsTold.reordered;
    };

    // The tree holds no clip: it hears nothing of them; the clips' listener hears the track's clips in another order.
    apply (reversed, "Reversed");
    CHECK (phazite::getClips (*song, track) == std::vector { clips[2], clips[1], clips[0] });
    CHECK (heard.properties.isEmpty());
    CHECK (heard.added + heard.removed + heard.moved == 0);
    CHECK (told.changed.empty());
    CHECK (told.reordered == std::vector { track });

    apply (before, "In order again");
    CHECK (phazite::getClips (*song, track) == clips);

    apply (renamed, "Renamed");
    INFO (heard.properties.joinIntoString (", ").toStdString());
    CHECK (heard.properties.isEmpty());
    CHECK (heard.added + heard.removed + heard.moved == 0);
    CHECK (told.changed == std::vector { clips[1] });
    CHECK (told.reordered.empty());
    CHECK (phazite::getClipValue (*song, clips[1])->name == "Q2");
    song->getValues()->removeClipsListener (&told);
}

TEST_CASE ("A version's alias groups, a member moved, one taken away and one added since, come back whole, MIDI and audio, their "
           "notes and sound one for each group; and one track's line gives them to the group's clips on other tracks")
{
    const TemporaryFolder temporary;
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 4);
    const auto keys = tracks[0], bass = tracks[1], loops = tracks[2], drums = tracks[3];
    const auto file = writeSound (temporary.folder.getChildFile ("Tone.wav"), 2.0);
    phazite::ClipID notes, onBass, later, sound, onDrums, laterSound;

    // On Keys a clip of two notes, an alias of it on Bass and another on Keys; on Loops an audio clip, an alias of it on
    // Drums and another on Loops.
    {
        const phazite::UndoStep step (*song, "Groups");
        notes = addMidiClip (*song, keys, { at (0.0), at (4.0) }, "Riff");
        addMidiNote (*song, notes, 48, at (0.0), lasting (1.0));
        addMidiNote (*song, notes, 55, at (2.0), lasting (1.0));
        onBass = phazite::copyClips (*song, { notes }, true).front();
        phazite::moveClip (*song, onBass, at (8.0), bass);
        later = phazite::copyClips (*song, { notes }, true).front();
        phazite::moveClip (*song, later, at (16.0), keys);

        sound = addAudioClip (*song, loops, "Tone", file, 0.0, 1.0);
        onDrums = phazite::copyClips (*song, { sound }, true).front();
        phazite::moveClip (*song, onDrums, at (8.0), drums);
        laterSound = phazite::copyClips (*song, { sound }, true).front();
        phazite::moveClip (*song, laterSound, at (16.0), loops);
    }

    REQUIRE (phazite::getAliasClips (*song, notes).size() == 3);
    REQUIRE (phazite::getAliasClips (*song, sound).size() == 3);
    const auto before = phazite::test::copyWholeSong (*song);
    const auto notesBefore = describeClipNotes (*song, notes);

    // Since: the alias on Bass moved onto Keys, the later one taken away, a new one on Bass, a note made quieter through the
    // new alias; the same for the audio group, its pitch changed through the new alias.
    phazite::ClipID newOnBass, newOnDrums;

    {
        const phazite::UndoStep step (*song, "Groups Changed");
        phazite::moveClip (*song, onBass, at (24.0), keys);
        phazite::deleteClips (*song, { later }, false);
        newOnBass = phazite::copyClips (*song, { notes }, true).front();
        phazite::moveClip (*song, newOnBass, at (32.0), bass);
        const auto quieter = phazite::getClipNotes (*song, newOnBass).front().id;
        phazite::setNoteVelocities (*song, { { newOnBass, quieter } }, 70);

        phazite::moveClip (*song, onDrums, at (24.0), loops);
        phazite::deleteClips (*song, { laterSound }, false);
        newOnDrums = phazite::copyClips (*song, { sound }, true).front();
        phazite::moveClip (*song, newOnDrums, at (32.0), drums);
        phazite::setClipPitch (*song, newOnDrums, 3.0f);
    }

    const auto after = phazite::test::copyWholeSong (*song);
    REQUIRE (describeClipNotes (*song, notes) != notesBefore);
    UndoCheck undo (*song);

    const auto groupsAreWhole = [&] (std::size_t members)
    {
        for (const auto clip : { notes, sound })
        {
            const auto group = phazite::getAliasClips (*song, clip);
            CHECK (group.size() == members);
            INFO (describeGroupNotes (*song, group).toStdString());
            CHECK (countDifferentNotes (*song, group) == 1);

            for (const auto member : group)
                CHECK (phazite::shareTheSound (phazite::copyWholeClip (*song, clip), phazite::copyWholeClip (*song, member)));
        }
    };

    bringBackChecked (*song, undo, before, "Before");
    groupsAreWhole (3);
    CHECK (phazite::getClipTrack (*song, onBass) == bass);
    CHECK (phazite::getClips (*song, keys) == std::vector { notes, later });
    CHECK_FALSE (phazite::getClipValue (*song, newOnBass).has_value());
    bringBackChecked (*song, undo, after, "After");
    groupsAreWhole (3);
    CHECK (phazite::getClips (*song, keys) == std::vector { notes, onBass });
    CHECK (phazite::getClipPitch (*song, sound) == doctest::Approx (3.0f));

    // Keys's line from before: its clips of the group are before's (the alias on Keys sent back to Bass, the later one back),
    // and so are the notes of the group's clip on Bass, which the line doesn't hold (known bug 234). Loops's: the sound of
    // the group's clips on Drums too.
    takeLineChecked (*song, undo, keys, before, "Keys's line",
                     [&]
                     {
                         CHECK (phazite::getClips (*song, keys) == std::vector { notes, later });
                         CHECK (phazite::getClipTrack (*song, onBass) == bass);
                         const auto group = phazite::getAliasClips (*song, notes);
                         CHECK (group.size() == 4);
                         INFO (describeGroupNotes (*song, group).toStdString());
                         CHECK (countDifferentNotes (*song, group) == 1);
                         CHECK (describeClipNotes (*song, newOnBass) == notesBefore);
                     });
    takeLineChecked (*song, undo, loops, before, "Loops's line",
                     [&]
                     {
                         CHECK (phazite::getClips (*song, loops) == std::vector { sound, laterSound });

                         for (const auto member : phazite::getAliasClips (*song, sound))
                             CHECK (phazite::getClipPitch (*song, member) == doctest::Approx (0.0f));
                     });

    undo.walkBackAndForth();
}

TEST_CASE ("A clip a version couldn't read stays as the song has it, where it has it, among its track's clips in the version's order")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto track = tracks[0];
    std::vector<phazite::ClipID> clips;

    {
        const phazite::UndoStep step (*song, "Clips");

        for (const auto* name : { "P", "Q", "R", "S" })
            clips.push_back (
                addMidiClip (*song, track, { at (4.0 * (double) clips.size()), at (4.0 * (double) clips.size() + 4.0) }, name));
    }

    // The version: R renamed, Q's piece lost (a stand-in of it in its place), S's lost and on the other track there, and its
    // clips in another order.
    auto version = phazite::test::copyWholeSong (*song);
    findClipElement (version, clips[2]).setProperty (schema::clipName, "R2", nullptr);
    findClipElement (version, clips[1]).setProperty (schema::versions::missing, true, nullptr);
    auto lost = findClipElement (version, clips[3]);
    lost.setProperty (schema::versions::missing, true, nullptr);
    lost.getParent().removeChild (lost, nullptr);
    phazite::getTrackElements (version)[1].appendChild (lost, nullptr);
    auto trackThere = phazite::getTrackElements (version).front();
    trackThere.moveChild (trackThere.indexOf (findClipElement (version, clips[2])), 0, nullptr);

    // Since: Q and S renamed in the song.
    {
        const phazite::UndoStep step (*song, "Rename");
        phazite::renameClips (*song, { clips[1] }, "Q now");
        phazite::renameClips (*song, { clips[3] }, "S now");
    }

    UndoCheck undo (*song);
    undo.check ("Restore",
                [&]
                {
                    const phazite::UndoStep step (*song, "Restore");
                    phazite::makeSongLike (*song, version);
                });

    // R first, as the version has it, renamed; Q as it is now, in its place in the version's order; P; and S where the song
    // has it, after them.
    CHECK (phazite::getClips (*song, track) == std::vector { clips[2], clips[0], clips[1], clips[3] });
    CHECK (phazite::getClipValue (*song, clips[2])->name == "R2");
    CHECK (phazite::getClipValue (*song, clips[1])->name == "Q now");
    CHECK (phazite::getClipValue (*song, clips[3])->name == "S now");
    CHECK (phazite::getClips (*song, tracks[1]).empty());
    CHECK_FALSE (phazite::writeElement (phazite::test::copyWholeSong (*song)).toString().contains (schema::versions::missing.toString()));
    checkWhole (*song);
    checkIDs (*song);
    undo.walkBackAndForth();
}

TEST_CASE ("A song made as it was copied, after a step that changed its clips was undone, finds no clip to change")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::ClipID clip, alias;

    {
        const phazite::UndoStep step (*song, "Clips");
        clip = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
        addMidiNote (*song, clip, 60, at (0.0), lasting (1.0));
        alias = phazite::copyClips (*song, { clip }, true).front();
        phazite::moveClip (*song, alias, at (8.0), tracks[1]);
    }

    const auto copy = phazite::test::copyWholeSong (*song);

    // A step that moves, renames and deletes clips, and adds a note, undone; and a track muted, which is no step.
    {
        const phazite::UndoStep step (*song, "Edit");
        phazite::moveClip (*song, clip, at (2.0), tracks[1]);
        phazite::renameClips (*song, { alias }, "Renamed");
        addMidiNote (*song, alias, 72, at (2.0), lasting (1.0));
        phazite::deleteClips (*song, { clip }, false);
    }

    phazite::undo (*song);
    phazite::setTrackMuted (*song, tracks[0], true);

    // The clip pass writes nothing here (its assertion: ApplySong.cpp, makeSongAsCopied()); the song is the copy, whole.
    phazite::makeSongAsCopied (*song, copy);
    CHECK (phazite::writeElement (phazite::test::copyWholeSong (*song)).toString() == phazite::writeElement (copy).toString());
    checkWhole (*song);
}

TEST_CASE ("A clip a track's line sends home goes among its home track's clips where the version has it, first, in the middle "
           "or last, whatever else the home track holds and wherever")
{
    // The home track's elements: its clips alone, or a device with them. One case of a device: made before the clips or
    // after them, it is in the same place, as the song holds its clips beside its tree and a whole copy writes them first.
    enum class Layout
    {
        clipsAlone,
        withDevice
    };

    for (const auto layout : { Layout::clipsAlone, Layout::withDevice })
    {
        for (std::size_t sent = 0; sent < 3; ++sent)
        {
            INFO ("layout " << (int) layout << ", clip " << sent << " sent home");
            auto song = phazite::createSong();
            const auto tracks = phazite::ensureTracks (*song, 2);
            const auto home = tracks[0], away = tracks[1];
            std::vector<phazite::ClipID> clips;
            phazite::DeviceID device;

            if (layout == Layout::withDevice)
            {
                const phazite::UndoStep step (*song, "Add Device");
                device = phazite::insertDeviceState (*song, home, phazite::makeOwnDeviceState (*song, phazite::devices::Equalizer::typeID));
                REQUIRE (device.isValid());
            }

            {
                const phazite::UndoStep step (*song, "Clips");

                for (const auto* name : { "P", "Q", "R" })
                {
                    const auto start = 4.0 * (double) clips.size();
                    clips.push_back (addMidiClip (*song, home, { at (start), at (start + 4.0) }, name));
                    addMidiNote (*song, clips.back(), 60 + (int) clips.size(), at (0.0), lasting (1.0));
                }
            }

            if (layout == Layout::withDevice)
                REQUIRE (phazite::findTrackState (*song, home).indexOf (phazite::findDeviceState (*song, device)) >= 0);

            const auto version = phazite::test::copyWholeSong (*song);

            {
                const phazite::UndoStep step (*song, "Move Clip");
                phazite::moveClip (*song, clips[sent], at (4.0 * (double) sent), away);
            }

            REQUIRE (phazite::getClipTrack (*song, clips[sent]) == away);
            UndoCheck undo (*song);

            // The away track's line: it holds no clip in the version, so the clip goes home, among the home track's clips
            // where the version has it.
            takeLineChecked (*song, undo, away, version, "The away track's line",
                             [&]
                             {
                                 CHECK (phazite::getClips (*song, home) == clips);
                                 CHECK (phazite::getClips (*song, away).empty());
                             });

            undo.walkBackAndForth();
        }
    }
}

TEST_CASE ("A clip a version holds as another kind under its ID, MIDI for audio or audio for MIDI, becomes the version's, whole")
{
    const TemporaryFolder temporary;
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto track = tracks[0];
    const auto file = writeSound (temporary.folder.getChildFile ("Tone.wav"), 2.0);
    phazite::ClipID clip;

    // An audio clip with what only an audio clip holds: fades, a gain, a warp marker.
    {
        const phazite::UndoStep step (*song, "Audio");
        clip = addAudioClip (*song, track, "Tone", file, 0.0, 1.0);
        phazite::setClipFade (*song, clip, true, 0.1);
        phazite::setClipFade (*song, clip, false, 0.2);
        phazite::setClipGain (*song, clip, -3.0f);
        REQUIRE (phazite::addWarpMarker (*song, clip, 0.5));
    }

    const auto asAudio = phazite::test::copyWholeSong (*song);

    // The other computer's song: on the same track, under the same ID, a MIDI clip with notes and with what only a MIDI clip
    // holds (a length and an offset into its notes). Made here as a clip of its own, its ID then given the audio clip's in the
    // copy, and the step undone.
    juce::ValueTree asMidi;

    {
        {
            const phazite::UndoStep step (*song, "MIDI");
            phazite::deleteClips (*song, { clip }, false);
            const auto midi = addMidiClip (*song, track, { at (2.0), at (10.0) }, "Riff");
            addMidiNote (*song, midi, 60, at (0.0), lasting (1.0));
            addMidiNote (*song, midi, 67, at (2.0), lasting (0.5));
            asMidi = phazite::test::copyWholeSong (*song);
            auto element = findClipElement (asMidi, midi);
            REQUIRE (element.isValid());
            element.setProperty (schema::clipID, (juce::int64) clip.getRaw(), nullptr);
        }

        phazite::undo (*song);
    }

    REQUIRE (phazite::getClipValue (*song, clip)->isAudio());
    REQUIRE (findClipElement (asMidi, clip)[schema::clipKind].toString() == schema::clipIsMidi);
    UndoCheck undo (*song);

    bringBackChecked (*song, undo, asMidi, "Audio to MIDI");
    CHECK (phazite::getClipValue (*song, clip)->isMidi());
    CHECK (phazite::getClipNotes (*song, clip).size() == 2);
    checkSong (*song);
    bringBackChecked (*song, undo, asAudio, "MIDI to audio");
    CHECK (phazite::getClipValue (*song, clip)->isAudio());
    CHECK (phazite::getWarpMarkers (*song, clip).size() == 1);
    checkSong (*song);
    undo.walkBackAndForth();
}

TEST_CASE ("A version in which a track is in another folder brings it back there, its clips with their IDs and notes, an alias's on "
           "a track that stays too, and its Undo and Redo put each back")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto stays = tracks[0], moving = tracks[1];
    phazite::ClipID riff, alias, own;
    phazite::TrackID folder;

    // On the track that stays, a clip of notes; on the one that moves, an alias of it and a clip of its own.
    {
        const phazite::UndoStep step (*song, "Clips");
        riff = addMidiClip (*song, stays, { at (0.0), at (4.0) }, "Riff");
        addMidiNote (*song, riff, 48, at (0.0), lasting (1.0));
        addMidiNote (*song, riff, 55, at (2.0), lasting (1.0));
        alias = phazite::copyClips (*song, { riff }, true).front();
        phazite::moveClip (*song, alias, at (8.0), moving);
        own = addMidiClip (*song, moving, { at (16.0), at (20.0) }, "Own");
        addMidiNote (*song, own, 72, at (1.0), lasting (1.0));
        folder = phazite::insertFolder (*song, moving, nullptr);
    }

    REQUIRE (folder.isValid());
    const auto outside = phazite::test::copyWholeSong (*song);
    const auto notesOutside = describeClipNotes (*song, riff), ownOutside = describeClipNotes (*song, own);

    // Since: the track moved into the folder, and a note added to the group through the alias and one to its own clip.
    {
        const phazite::UndoStep step (*song, "Into Folder");
        phazite::moveTrackIntoFolder (*song, moving, folder);
        addMidiNote (*song, alias, 60, at (3.0), lasting (0.5));
        addMidiNote (*song, own, 74, at (2.0), lasting (1.0));
    }

    REQUIRE (phazite::getParentFolder (*song, moving) == folder);
    const auto inside = phazite::test::copyWholeSong (*song);
    const auto notesInside = describeClipNotes (*song, riff), ownInside = describeClipNotes (*song, own);
    REQUIRE (notesInside != notesOutside);
    UndoCheck undo (*song);

    // Each clip's notes: the group's, alike on both of its clips, and the own clip's.
    const auto checkNotes = [&] (const juce::String& group, const juce::String& ownNotes)
    {
        CHECK (describeClipNotes (*song, riff) == group);
        CHECK (describeClipNotes (*song, alias) == group);
        CHECK (describeClipNotes (*song, own) == ownNotes);
        CHECK (phazite::getAliasClips (*song, riff).size() == 2);
    };

    // Each way: the version's after the step, the song before it after its Undo, the version's again after its Redo.
    const auto apply = [&] (const juce::ValueTree& version, const juce::ValueTree& before, phazite::TrackID parent,
                            const juce::String& group, const juce::String& ownNotes, const juce::String& groupBefore,
                            const juce::String& ownBefore, const juce::String& what)
    {
        INFO (what.toStdString());
        undo.check (what,
                    [&]
                    {
                        const phazite::UndoStep step (*song, what);
                        phazite::makeSongLike (*song, version);
                        phazite::removeBrokenReferences (*song);
                    });
        checkSongIs (*song, version);
        CHECK (phazite::getParentFolder (*song, moving) == parent);
        CHECK (phazite::getClips (*song, moving) == std::vector { alias, own });
        checkNotes (group, ownNotes);
        undo.stepBackOrForth ("Undo " + what, [&] { phazite::undo (*song); });
        checkSongIs (*song, before);
        checkNotes (groupBefore, ownBefore);
        undo.stepBackOrForth ("Redo " + what, [&] { phazite::redo (*song); });
        checkSongIs (*song, version);
        checkNotes (group, ownNotes);
    };

    apply (outside, inside, {}, notesOutside, ownOutside, notesInside, ownInside, "Out of the folder");
    apply (inside, outside, folder, notesInside, ownInside, notesOutside, ownOutside, "Into the folder");
    undo.walkBackAndForth();
}

// An audio clip the version holds without a name of its own (renamed to nothing, which takes the property away), deleted
// and brought back: the song is the version, the clip holding no name. When the song's tree held its clips, the writer of a
// new audio clip's element gave it an empty name, which the comparison with the version counted as a change; a clip's
// value writes its name only while it is set (SongValues.h: writeClipElement()).
TEST_CASE ("An audio clip a version holds without a name comes back as the version's, without one")
{
    const TemporaryFolder temporary;
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1)[0];
    const auto file = writeSound (temporary.folder.getChildFile ("Tone.wav"), 2.0);
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*song, "Audio");
        clip = addAudioClip (*song, track, "Tone", file, 0.0, 1.0);
    }

    phazite::renameClips (*song, { clip }, "");
    const auto version = phazite::test::copyWholeSong (*song);
    REQUIRE_FALSE (findClipElement (version, clip).hasProperty (schema::clipName));

    {
        const phazite::UndoStep step (*song, "Delete Clip");
        phazite::deleteClips (*song, { clip }, false);
    }

    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, version);
    }

    // Without a name, as the version holds it (a clip's name is written only while it is set): the song is the version.
    const auto broughtBack = phazite::copyWholeClip (*song, clip);
    INFO (phazite::writeElement (broughtBack).toString().toStdString());
    CHECK_FALSE (broughtBack.hasProperty (schema::clipName));
    checkSongIs (*song, version);
}

TEST_CASE ("Bring Back of the version the song is, and a clip or its notes set to what they hold, make no undo step, alias "
           "groups of MIDI and audio clips too")
{
    // An alias group's clips hold one value of what they share (SongValues.h: ClipSound, and one ClipNotes): a value alike in
    // another value is no change of the song's, which an area gives them without a step of its own; in the tree, a step
    // without actions was none (JUCE drops it).
    const TemporaryFolder temporary;
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto file = writeSound (temporary.folder.getChildFile ("Tone.wav"), 2.0);
    phazite::ClipID riff, tone, riffAlias, toneAlias;

    {
        const phazite::UndoStep step (*song, "Clips");
        riff = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
        addMidiNote (*song, riff, 60, at (0.0), lasting (1.0));
        tone = addAudioClip (*song, tracks[1], "Tone", file, 0.0, 1.0);
        REQUIRE (phazite::addWarpMarker (*song, tone, 0.5)); // so that its sound holds markers: a value with an identity
        riffAlias = phazite::copyClips (*song, { riff }, true).at (0);
        toneAlias = phazite::copyClips (*song, { tone }, true).at (0);
        phazite::moveClip (*song, riffAlias, at (8.0), tracks[0]);
        phazite::moveClip (*song, toneAlias, at (8.0), tracks[1]);
    }

    REQUIRE (phazite::getAliasClips (*song, riff).size() == 2);
    REQUIRE (phazite::getAliasClips (*song, tone).size() == 2);
    REQUIRE_FALSE (phazite::getWarpMarkers (*song, tone).empty());
    song->getUndoManager().clearUndoHistory();
    const auto version = phazite::test::copyWholeSong (*song);

    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, version);
    }

    CHECK_FALSE (phazite::canUndo (*song));
    checkSongIs (*song, version);

    // Each clip set to the value it holds; the audio group's clips given their sound alike in another value (its markers a
    // vector of their own), and the MIDI group's its notes alike in another value.
    {
        const phazite::UndoStep step (*song, "Set");

        for (const auto clip : { riff, riffAlias, tone, toneAlias })
            phazite::setClip (*song, phazite::getClipValue (*song, clip).value());

        const auto held = phazite::getClipValue (*song, tone).value();
        auto sound = held.sound;
        sound.warps = immer::flex_vector<phazite::WarpValue> (held.sound.warps.begin(), held.sound.warps.end());
        REQUIRE_FALSE (sound.isSameAs (held.sound));
        REQUIRE (sound == held.sound);

        for (const auto clip : { tone, toneAlias })
        {
            const auto each = phazite::getClipValue (*song, clip).value();
            auto alike = each;
            alike.sound = sound;
            phazite::setClip (*song, each, alike);
        }

        const auto notes = phazite::getClipNotesValue (*song, riff);
        auto alike = phazite::makeClipNotes (std::vector<phazite::NoteValues> (notes.notes.begin(), notes.notes.end()));
        REQUIRE_FALSE (alike.isSameAs (notes));
        REQUIRE (alike == notes);
        phazite::setClipNotesValue (*song, riff, alike);
    }

    CHECK_FALSE (phazite::canUndo (*song));
    checkSongIs (*song, version);

    // The groups hold one value each: the one given.
    CHECK (phazite::getClipValue (*song, toneAlias)->sound.isSameAs (phazite::getClipValue (*song, tone)->sound));
    CHECK (phazite::getClipNotesValue (*song, riffAlias).isSameAs (phazite::getClipNotesValue (*song, riff)));
    phazite::test::checkValues (*song);
}
