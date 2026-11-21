// SPDX-License-Identifier: AGPL-3.0-or-later
// Going back to a version (docs/design/versions.md: Going back, step V2): Bring Back This Version, Put Back and Take from This
// Version, each one undo step that what plays follows, with the fewest edits, and what isn't counted left as it is.
#include "GoldenFiles.h"
#include "RenderHarness.h"
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
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
// rule. Or by every rule but that the clips of an alias group hold the same notes, for the edits that take one track's line
// back alone, which break it today: known bug 234 (docs/known-bugs.md), which the findings below check. A group of one clip,
// or of clips of different kinds, is still a problem then.
void checkWhole (const phazite::Song& song, bool notesOfAliasesMayDiffer = false)
{
    for (const auto& problem : findSongProblems (*copyAsOpened (song)))
        if (! (notesOfAliasesMayDiffer && juce::String (problem).contains ("its notes and values differ")))
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
                    if (phazite::findClipState (song, clips.front())[schema::clipKind].toString() == schema::clipIsMidi)
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
    // is checked after it and its Redo with the notes of an alias group's clips allowed to differ, which it breaks today
    // (known bug 234): the FINDING cases after this one check that of Put Back and of a line restored.
    const auto goBack = [&] (const juce::String& what, const std::function<bool()>& command, const std::function<void()>& gives,
                             bool notesOfAliasesMayDiffer = false)
    {
        INFO (what.toStdString());
        undo.check (what,
                    [&]
                    {
                        REQUIRE (phazite::test::finished (s.context, command));
                        gives();
                    });
        checkWhole (song, notesOfAliasesMayDiffer);
        undo.stepBackOrForth ("Undo " + what, [&song] { phazite::undo (song); });
        checkWhole (song);
        undo.stepBackOrForth ("Redo " + what, [&song] { phazite::redo (song); });
        checkWhole (song, notesOfAliasesMayDiffer);
        undo.stepBackOrForth ("Undo " + what + " again", [&song] { phazite::undo (song); });
    };

    // Put Back, a line each, against the last version (Second): Keys (the alias that the notes were edited through), then
    // Bass (the group's first and last).
    const auto keysLine = lineOf ("Keys");
    goBack ("Put Back Keys", [&] { return phazite::takeBack (s.context, keysLine); }, notesAre (n.keys, n.secondKeys), true);
    const auto bassLine = lineOf ("Bass");
    goBack ("Put Back Bass", [&] { return phazite::takeBack (s.context, bassLine); }, notesAre (n.bass, n.secondBass), true);

    // Discard All Changes, and Restore This Version on the last version and on the one before, by the command.
    goBack ("Discard All Changes", [&] { return phazite::discardAllChanges (s.context); }, songIs (n.secondText));
    goBack ("Restore Second", [&] { return phazite::bringBackVersion (s.context, n.second); }, songIs (n.secondText));
    phazite::selectVersion (s.context, n.first);
    REQUIRE (phazite::canBringBack (s.context));
    goBack ("version.bringBack", [&] { return commands.invoke ("version.bringBack"); }, songIs (n.firstText));

    // Take from This Version: a line of what differs from First, taken from it.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, n.first); }));
    const auto keysFromFirst = lineOf ("Keys");
    goBack ("Take Keys from First", [&] { return phazite::takeBack (s.context, keysFromFirst); }, notesAre (n.keys, n.firstKeys), true);
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::compareWithVersion (s.context, {}); }));

    // A line of what Second changed, restored from First, and one from Second.
    const auto secondsLines = readVersionChanges (s.context, n.second);
    const auto keysOfSecond = findLine (secondsLines, "Keys"), bassOfSecond = findLine (secondsLines, "Bass");
    INFO ("Second's lines: " << describeChanges (secondsLines).toStdString());
    REQUIRE (keysOfSecond.has_value());
    REQUIRE (bassOfSecond.has_value());
    goBack (
        "Restore Keys from First", [&] { return phazite::restoreFromVersion (s.context, n.first, *keysOfSecond); },
        notesAre (n.keys, n.firstKeys), true);
    goBack (
        "Restore Bass from Second", [&] { return phazite::restoreFromVersion (s.context, n.second, *bassOfSecond); },
        notesAre (n.bass, n.secondBass), true);

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

// FINDING (known bug 234): Put Back of one track's line (takeBack(); Take from This Version is the same) makes that track's
// clips of an alias group what they were and leaves the group's clips on other tracks as they are now, so that the group's
// clips hold different notes, which Schema.h (clipLink) says they never do. Once fixed, the group's other clips take the
// restored notes, as a merge gives every clip of a group its keeper's notes (docs/design/versions.md).
TEST_CASE ("Put Back of one track's line keeps the clips of an alias group on other tracks holding the same notes" * doctest::should_fail())
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

// FINDING (known bug 234): a line of a version restored alone (restoreFromVersion(), version.restoreChange) does the same:
// the track's clips of an alias group become the version's, the group's clips on other tracks stay as they are, holding
// other notes. Once fixed, the group's other clips take the restored notes, as a merge gives every clip of a group its
// keeper's notes (docs/design/versions.md).
TEST_CASE ("A track's line restored from a version keeps the clips of an alias group on other tracks holding the same notes"
           * doctest::should_fail())
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
