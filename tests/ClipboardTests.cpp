// SPDX-License-Identifier: AGPL-3.0-or-later
// Cut, Copy and Paste (src/model/arrangement/internal/Clipboard.h; roadmap step 3.7): clips, a time range's clips and
// notes, put in at the playhead, each paste one named undo step that selects what it put in, with the automation under
// clips going back onto their own tracks.
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

namespace
{
using phazite::BeatPosition;
using phazite::test::at;
using phazite::test::lasting;

// Two tracks, the clipboard's commands, and on the first track a clip from beat 0 to beat 4 with two notes, under which
// the track's volume rises from 0.2 at beat 1 to 0.8 at beat 3.
struct Song
{
    Song()
    {
        tracks = phazite::ensureTracks (getSong(), 2);
        setSong (context, songPtr.get());
        phazite::addClipboardCommands (registry, context);
        clip = phazite::insertMidiClip (getSong(), tracks[0], { at (0.0), at (4.0) }, nullptr);
        phazite::addNote (getSong(), clip, 60, at (1.0), lasting (0.5), 100);
        phazite::addNote (getSong(), clip, 64, at (1.5), lasting (0.5), 90);
        volume = phazite::getVolume (context, tracks[0]).getRef();
        phazite::addAutomationPoint (context, volume, { BeatPosition::fromBeats (1.0), 0.2f });
        phazite::addAutomationPoint (context, volume, { BeatPosition::fromBeats (3.0), 0.8f });
        getSong().getUndoManager().clearUndoHistory();
    }

    ~Song() { setSong (context, nullptr); }

    phazite::Song& getSong() const { return *songPtr; }

    void playheadAt (double beat) { phazite::movePlayhead (getSong(), at (beat)); }

    bool isEnabled (const char* id) const { return phazite::CommandRegistry::isEnabled (*registry.find (id)); }

    float volumeAt (double beat) const
    {
        return phazite::getAutomationValues (context, volume, { BeatPosition::fromBeats (beat), BeatPosition::fromBeats (beat) }, 1)
            .front();
    }

    // A track's clips, from the earliest.
    std::vector<phazite::ClipID> clipsOf (int track) const
    {
        auto clips = phazite::getClips (getSong(), tracks[(size_t) track]);
        std::sort (clips.begin(), clips.end(),
                   [this] (phazite::ClipID a, phazite::ClipID b) { return rangeOf (a).getStart() < rangeOf (b).getStart(); });
        return clips;
    }

    phazite::BeatRange rangeOf (phazite::ClipID id) const { return phazite::getClipRange (getSong(), id); }

    // A clip's notes, from the earliest.
    std::vector<phazite::NoteData> notesOf (phazite::ClipID id) const
    {
        auto notes = phazite::getClipNotes (getSong(), id);
        std::sort (notes.begin(), notes.end(), [] (const phazite::NoteData& a, const phazite::NoteData& b) { return a.start < b.start; });
        return notes;
    }

    juce::String describeUndo() const { return phazite::describeUndo (songPtr.get()); }

    phazite::Song::Ptr songPtr = phazite::createSong(); // 120 BPM, 4/4
    std::vector<phazite::TrackID> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ClipID clip;
    phazite::ParameterRef volume;
};
} // namespace

TEST_CASE ("Copied clips paste at the playhead on the selected track, as copies, selected, in one undo step")
{
    Song song;
    auto& selection = getSelection (song.context);
    CHECK_FALSE (song.isEnabled ("edit.copy"));
    CHECK_FALSE (song.isEnabled ("edit.paste"));

    // Copying changes nothing in the song.
    selection.selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.copy"));
    CHECK_FALSE (song.getSong().getUndoManager().canUndo());
    CHECK (song.isEnabled ("edit.paste"));

    selection.selectOnly (song.tracks[1]);
    song.playheadAt (8.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste Clip"));

    const auto pasted = song.clipsOf (1);
    REQUIRE (pasted.size() == 1);
    const auto copy = pasted.front();
    REQUIRE (phazite::isMidiState (phazite::findClipState (song.getSong(), copy)));
    CHECK (song.rangeOf (copy).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (song.rangeOf (copy).getEnd().inBeats() == doctest::Approx (12.0));
    CHECK (phazite::getClipNotes (song.getSong(), copy).size() == 2);
    CHECK_FALSE (phazite::hasAliases (song.getSong(), { copy })); // a copy, not an alias
    CHECK (selection.isSelected (copy));
    CHECK_FALSE (selection.isSelected (song.clip));

    // On another track, the automation under it stays where it was.
    CHECK (song.volumeAt (9.0) == doctest::Approx (song.volumeAt (20.0)));

    phazite::undo (song.getSong());
    CHECK (song.clipsOf (1).empty());
}

TEST_CASE ("Clips copied from more tracks than there are below the selected one paste onto tracks added for them, never a return")
{
    // Clips from two tracks pasted with the last track selected: the second went onto a return, where it neither showed nor
    // played, yet counted towards the song's end (known bug 48).
    Song song;
    const auto reverb = phazite::insertReturnTrack (song.getSong(), "Reverb", nullptr);
    REQUIRE (reverb.isValid());
    const auto second = phazite::insertMidiClip (song.getSong(), song.tracks[1], { at (0.0), at (2.0) }, nullptr);
    REQUIRE (second.isValid());
    song.getSong().getUndoManager().clearUndoHistory();
    auto& selection = getSelection (song.context);
    selection.selectOnly (song.clip);
    selection.addToSelection (second);
    REQUIRE (song.registry.invoke ("edit.copy"));

    selection.selectOnly (song.tracks[1]);
    song.playheadAt (8.0);
    const auto tracksBefore = phazite::getTrackIDs (song.getSong());
    phazite::test::UndoCheck undo (song.getSong());
    undo.command (song.registry, "edit.paste");

    // The first on the selected track, the second on a track added after it; the return holds none.
    CHECK (phazite::getClips (song.getSong(), reverb).empty());
    REQUIRE (song.clipsOf (1).size() == 2);
    CHECK (song.rangeOf (song.clipsOf (1)[1]).getStart().inBeats() == doctest::Approx (8.0));

    const auto tracksAfter = phazite::getTrackIDs (song.getSong());
    REQUIRE (tracksAfter.size() == tracksBefore.size() + 1);
    std::vector<phazite::TrackID> added;
    std::copy_if (tracksAfter.begin(), tracksAfter.end(), std::back_inserter (added), [&tracksBefore] (phazite::TrackID id)
                  { return std::find (tracksBefore.begin(), tracksBefore.end(), id) == tracksBefore.end(); });
    REQUIRE (added.size() == 1);
    CHECK_FALSE (phazite::isReturnTrack (song.getSong(), added[0]));
    REQUIRE (phazite::getClips (song.getSong(), added[0]).size() == 1);
    CHECK (song.rangeOf (phazite::getClips (song.getSong(), added[0])[0]).getStart().inBeats() == doctest::Approx (8.0));

    // Nor can a clip be moved onto a return.
    phazite::moveClip (song.getSong(), second, at (16.0), reverb);
    CHECK (phazite::getClips (song.getSong(), reverb).empty());
    undo.walkBackAndForth();
}

TEST_CASE ("Cut clips paste back onto their own track with the automation that was under them, in place of what is there")
{
    Song song;
    auto& selection = getSelection (song.context);
    const auto later = phazite::insertMidiClip (song.getSong(), song.tracks[0], { at (10.0), at (14.0) }, nullptr);
    REQUIRE (later.isValid());
    song.getSong().getUndoManager().clearUndoHistory();

    selection.selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.cut"));
    CHECK (song.describeUndo() == juce::String ("Undo Cut Clip"));
    REQUIRE (song.clipsOf (0).size() == 1);
    CHECK (song.volumeAt (2.0) == doctest::Approx (0.5f)); // as Delete leaves it: the clip goes, its automation stays

    // Nothing selected: back onto the track it came from, at the playhead, over the start of the later clip.
    selection.deselectAll();
    song.playheadAt (8.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    const auto clips = song.clipsOf (0);
    REQUIRE (clips.size() == 2);
    CHECK (song.rangeOf (clips[0]).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (song.rangeOf (clips[0]).getEnd().inBeats() == doctest::Approx (12.0));
    CHECK (clips[1] == later);
    CHECK (song.rangeOf (later).getStart().inBeats() == doctest::Approx (12.0)); // trimmed: nothing plays twice
    CHECK (song.volumeAt (9.0) == doctest::Approx (0.2f));
    CHECK (song.volumeAt (11.0) == doctest::Approx (0.8f));
    CHECK (song.volumeAt (1.0) == doctest::Approx (0.2f)); // and where it was copied from, still

    phazite::undo (song.getSong());
    CHECK (song.clipsOf (0).size() == 1);
    CHECK (song.rangeOf (later).getStart().inBeats() == doctest::Approx (10.0));
}

TEST_CASE ("A copied time range pastes its clips cut to its edges, and becomes the range")
{
    Song song;
    auto& range = getRangeSelection (song.context);
    range.set ({ at (1.0), at (3.0) }, { song.tracks[0] });
    REQUIRE (song.registry.invoke ("edit.copy"));
    CHECK (song.clipsOf (0).size() == 1); // the clip itself isn't cut

    song.playheadAt (16.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste Range"));
    const auto clips = song.clipsOf (0);
    REQUIRE (clips.size() == 2);
    CHECK (song.rangeOf (clips[1]).getStart().inBeats() == doctest::Approx (16.0));
    CHECK (song.rangeOf (clips[1]).getEnd().inBeats() == doctest::Approx (18.0));
    CHECK (range.getRange().getStart().inBeats() == doctest::Approx (16.0));
    CHECK (range.getRange().getEnd().inBeats() == doctest::Approx (18.0));
    CHECK (song.volumeAt (17.0) == doctest::Approx (song.volumeAt (2.0)));
}

TEST_CASE ("Copied notes paste at the playhead in the clip the piano roll shows there, else as far into its first clip")
{
    Song song;
    auto& songSelection = song.context.getSongSelection();
    const auto from = song.clip;
    const auto to = phazite::insertMidiClip (song.getSong(), song.tracks[1], { at (8.0), at (12.0) }, nullptr);
    REQUIRE (to.isValid());
    song.getSong().getUndoManager().clearUndoHistory();

    songSelection.setNoteClips ({ from });
    songSelection.selectNotes (phazite::getRefs (phazite::getClipNotes (song.getSong(), from)));
    REQUIRE (songSelection.getNotes().size() == 2);
    REQUIRE (song.registry.invoke ("edit.copy"));

    // The piano roll shows the other clip; the playhead isn't in it: a beat into it, as the first note was into its own.
    songSelection.setNoteClips ({ to });
    song.playheadAt (0.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste 2 Notes"));
    const auto pasted = song.notesOf (to);
    REQUIRE (pasted.size() == 2);
    CHECK (pasted[0].start.inBeats() == doctest::Approx (9.0));
    CHECK (pasted[1].start.inBeats() == doctest::Approx (9.5));
    CHECK (pasted[1].pitch == 64);
    CHECK (songSelection.getNotes().size() == 2); // the pasted notes

    // With the playhead in it, there.
    song.playheadAt (10.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    const auto all = song.notesOf (to);
    REQUIRE (all.size() == 4);
    CHECK (all[2].start.inBeats() == doctest::Approx (10.0));
    CHECK (all[3].start.inBeats() == doctest::Approx (10.5));
}

TEST_CASE ("Another project empties the clipboard")
{
    Song song;
    getSelection (song.context).selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.copy"));
    CHECK (song.isEnabled ("edit.paste"));

    auto another = phazite::createSong();
    setSong (song.context, another.get());
    CHECK_FALSE (song.isEnabled ("edit.paste"));
    setSong (song.context, song.songPtr.get());
    CHECK_FALSE (song.isEnabled ("edit.paste"));
}

TEST_CASE ("Tracks added for pasted clips go after the last track, outside any folder and before the returns")
{
    // The last track in a folder: the tracks added for the clips went into the folder after it, a collapsed one too.
    Song song;
    auto& document = song.getSong();
    const auto folder = phazite::insertFolder (document, song.tracks[1], nullptr);
    phazite::moveTracksTo (document, { song.tracks[1] }, folder, phazite::TrackDrop::into);
    const auto reverb = phazite::insertReturnTrack (document, "Reverb", nullptr);
    const auto second = phazite::insertMidiClip (document, song.tracks[1], { at (0.0), at (2.0) }, nullptr);
    REQUIRE (phazite::getParentFolder (document, song.tracks[1]) == folder);
    document.getUndoManager().clearUndoHistory();

    auto& selection = getSelection (song.context);
    selection.selectOnly (song.clip);
    selection.addToSelection (second);
    REQUIRE (song.registry.invoke ("edit.copy"));
    selection.selectOnly (song.tracks[1]);
    song.playheadAt (8.0);
    const auto before = phazite::getTrackIDs (document);
    phazite::test::UndoCheck undo (document);
    undo.command (song.registry, "edit.paste");

    const auto after = phazite::getTrackIDs (document);
    REQUIRE (after.size() == before.size() + 1);
    const auto added = after[after.size() - 2]; // before the return, which stays last
    CHECK (after.back() == reverb);
    CHECK_FALSE (phazite::getParentFolder (document, added).isValid());
    CHECK (phazite::getClips (document, added).size() == 1);
    CHECK (song.clipsOf (1).size() == 2);
    undo.walkBackAndForth();
}

TEST_CASE ("With a folder or a return selected, Paste puts clips from the first track below it, or on a track added at the end")
{
    // A folder or a return holds no clips: with one selected, Paste did nothing.
    Song song;
    auto& document = song.getSong();
    const auto folder = phazite::insertFolder (document, song.tracks[0], nullptr);
    phazite::moveTracksTo (document, { song.tracks[1] }, folder, phazite::TrackDrop::into);
    const auto reverb = phazite::insertReturnTrack (document, "Reverb", nullptr);
    document.getUndoManager().clearUndoHistory();

    auto& selection = getSelection (song.context);
    selection.selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.copy"));
    song.playheadAt (8.0);
    phazite::test::UndoCheck undo (document);

    // The folder: onto the first track in it.
    selection.selectOnly (folder);
    REQUIRE (song.isEnabled ("edit.paste"));
    undo.command (song.registry, "edit.paste");
    REQUIRE (song.clipsOf (1).size() == 1);
    CHECK (song.rangeOf (song.clipsOf (1)[0]).getStart().inBeats() == doctest::Approx (8.0));

    // The return, below which no track holds clips: onto a track added for them before it.
    const auto before = phazite::getTrackIDs (document);
    selection.selectOnly (reverb);
    REQUIRE (song.isEnabled ("edit.paste"));
    undo.command (song.registry, "edit.paste");
    const auto after = phazite::getTrackIDs (document);
    REQUIRE (after.size() == before.size() + 1);
    CHECK (after.back() == reverb);
    const auto added = after[after.size() - 2];
    CHECK_FALSE (phazite::isReturnTrack (document, added));
    CHECK_FALSE (phazite::getParentFolder (document, added).isValid());
    REQUIRE (phazite::getClips (document, added).size() == 1);
    CHECK (song.rangeOf (phazite::getClips (document, added)[0]).getStart().inBeats() == doctest::Approx (8.0));
    undo.walkBackAndForth();
}

TEST_CASE ("Pasted clips go to the next track below that holds their kind, or to a track added for them: never notes beside audio")
{
    // Paste chose tracks by their order alone: notes copied and pasted with an audio track selected went onto it, beside
    // its audio, and audio onto a track with an instrument, which silenced it.
    Song song;
    auto& document = song.getSong();
    phazite::test::TemporaryFolder temp;
    const auto audioTrack = phazite::insertTrack (document, song.tracks[1], nullptr);
    const auto audio =
        phazite::importFile (document, audioTrack, phazite::test::writeSound (temp.folder.getChildFile ("Loop.wav"), 1.0), at (0.0));
    REQUIRE (phazite::isAudioState (phazite::findClipState (document, audio)));
    phazite::insertMidiClip (document, song.tracks[1], { at (0.0), at (2.0) }, nullptr); // notes there too
    document.getUndoManager().clearUndoHistory();
    auto& selection = getSelection (song.context);
    phazite::test::UndoCheck undo (document);

    // The notes, with the audio track selected: onto a track added for them, after the last.
    selection.selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.copy"));
    selection.selectOnly (audioTrack);
    song.playheadAt (8.0);
    const auto before = phazite::getTrackIDs (document);
    undo.command (song.registry, "edit.paste");
    CHECK (phazite::getClips (document, audioTrack).size() == 1);
    const auto after = phazite::getTrackIDs (document);
    REQUIRE (after.size() == before.size() + 1);
    REQUIRE (phazite::getClips (document, after.back()).size() == 1);
    CHECK (phazite::isMidiState (phazite::findClipState (document, phazite::getClips (document, after.back())[0])));

    // The audio, with the first track selected, which holds notes: onto the audio track below it.
    selection.selectOnly (audio);
    REQUIRE (song.registry.invoke ("edit.copy"));
    selection.selectOnly (song.tracks[0]);
    song.playheadAt (16.0);
    undo.command (song.registry, "edit.paste");
    CHECK (phazite::getClips (document, song.tracks[0]).size() == 1);
    CHECK (phazite::getClips (document, audioTrack).size() == 2);

    // Nor does a move put a clip beside clips of the other kind.
    undo.check ("Move Clip",
                [&]
                {
                    const phazite::UndoStep step (document, "Move Clip");
                    phazite::moveClip (song.context, song.clip, at (32.0), audioTrack);
                });
    CHECK (phazite::getClipTrack (document, song.clip) == song.tracks[0]);
    undo.walkBackAndForth();
}

TEST_CASE ("A linked clip pastes as a copy of its own, with its notes, which an edit to the clip it was linked to leaves alone")
{
    Song song;
    const auto linked = phazite::copyClips (song.getSong(), { song.clip }, true);
    REQUIRE (linked.size() == 1);
    REQUIRE (phazite::hasAliases (song.getSong(), linked));
    song.getSong().getUndoManager().clearUndoHistory();

    auto& selection = getSelection (song.context);
    selection.selectOnly (linked.front());
    REQUIRE (song.registry.invoke ("edit.copy"));
    selection.selectOnly (song.tracks[1]);
    song.playheadAt (16.0);
    phazite::test::UndoCheck check (song.getSong());
    check.command (song.registry, "edit.paste");

    const auto pasted = song.clipsOf (1);
    REQUIRE (pasted.size() == 1);
    CHECK_FALSE (phazite::hasAliases (song.getSong(), pasted));
    CHECK (song.notesOf (pasted.front()).size() == 2);
    CHECK (song.rangeOf (pasted.front()).getStart().inBeats() == doctest::Approx (16.0));

    // The clips still linked stay linked; a note added to them doesn't reach the copy.
    CHECK (phazite::hasAliases (song.getSong(), { song.clip }));
    check.checkSeveral ("a note in the linked clips",
                        [&song] { phazite::addNote (song.getSong(), song.clip, 72, at (2.0), lasting (0.5), 100); });
    CHECK (song.notesOf (linked.front()).size() == 3);
    CHECK (song.notesOf (pasted.front()).size() == 2);
    check.walkBackAndForth();
    phazite::test::checkSong (song.getSong());
}

TEST_CASE ("An audio clip pastes with its file and the part of it it plays")
{
    Song song;
    const phazite::test::TemporaryFolder temp;
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Take.wav"), 4.0);
    const auto take = phazite::test::addAudioClip (song.getSong(), song.tracks[0], "Take", file, 6.0, 1.5, 0.5); // beats 12 to 15
    REQUIRE (take.isValid());
    song.getSong().getUndoManager().clearUndoHistory();

    auto& selection = getSelection (song.context);
    selection.selectOnly (take);
    REQUIRE (song.registry.invoke ("edit.copy"));
    selection.selectOnly (song.tracks[1]);
    song.playheadAt (20.0);
    phazite::test::UndoCheck check (song.getSong());
    check.command (song.registry, "edit.paste");

    const auto pasted = song.clipsOf (1);
    REQUIRE (pasted.size() == 1);
    const auto pastedState = phazite::findClipState (song.getSong(), pasted.front());
    REQUIRE (phazite::isAudioState (pastedState));
    CHECK (phazite::getClipFile (song.getSong(), pastedState) == file);
    CHECK (song.rangeOf (pasted.front()).getStart().inBeats() == doctest::Approx (20.0));
    CHECK (song.rangeOf (pasted.front()).getLength().inBeats() == doctest::Approx (song.rangeOf (take).getLength().inBeats()));
    CHECK (phazite::getFileTimeAt (song.getSong(), pasted.front(), at (21.0))
           == doctest::Approx (phazite::getFileTimeAt (song.getSong(), take, at (13.0))));
    CHECK (song.clipsOf (0).size() == 2); // the one copied stays
    phazite::test::checkSong (song.getSong());
}

TEST_CASE ("Paste twice in one place leaves one copy there, and in two places two, each with IDs of their own")
{
    Song song;
    auto& selection = getSelection (song.context);
    selection.selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.copy"));
    selection.selectOnly (song.tracks[1]);
    song.playheadAt (8.0);
    phazite::test::UndoCheck check (song.getSong());

    check.command (song.registry, "edit.paste");
    const auto first = song.clipsOf (1);
    REQUIRE (first.size() == 1);

    // The same again: in the first copy's place, so that nothing plays twice.
    selection.selectOnly (song.tracks[1]);
    check.command (song.registry, "edit.paste");
    const auto second = song.clipsOf (1);
    REQUIRE (second.size() == 1);
    CHECK (second.front() != first.front());
    CHECK (song.rangeOf (second.front()).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (song.notesOf (second.front()).size() == 2);

    // Elsewhere: a second copy beside it.
    selection.selectOnly (song.tracks[1]);
    song.playheadAt (12.0);
    check.command (song.registry, "edit.paste");
    const auto both = song.clipsOf (1);
    REQUIRE (both.size() == 2);
    CHECK (song.rangeOf (both[1]).getStart().inBeats() == doctest::Approx (12.0));
    CHECK (phazite::test::findIDProblems (song.getSong().getState()).empty());
    check.walkBackAndForth();
}

TEST_CASE ("Cut and then Undo puts the clip and the automation under it back as they were, and the cut clip still pastes")
{
    Song song;
    auto& selection = getSelection (song.context);
    selection.selectOnly (song.clip);
    phazite::test::UndoCheck check (song.getSong());
    check.command (song.registry, "edit.cut");
    CHECK (song.clipsOf (0).empty());

    auto& songState = song.getSong();
    check.stepBackOrForth ("Undo Cut", [&songState] { phazite::undo (songState); });
    REQUIRE (song.clipsOf (0) == std::vector<phazite::ClipID> { song.clip });
    CHECK (song.notesOf (song.clip).size() == 2);
    CHECK (song.volumeAt (1.0) == doctest::Approx (0.2f));
    CHECK (song.volumeAt (3.0) == doctest::Approx (0.8f));

    // What was cut is still on the clipboard: pasted, a copy beside the clip that came back.
    selection.selectOnly (song.tracks[0]);
    song.playheadAt (8.0);
    check.command (song.registry, "edit.paste");
    const auto clips = song.clipsOf (0);
    REQUIRE (clips.size() == 2);
    CHECK (clips[0] == song.clip);
    CHECK (song.rangeOf (clips[1]).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (song.volumeAt (9.0) == doctest::Approx (0.2f));
    check.walkBackAndForth();
}

TEST_CASE ("A time range across two tracks pastes on both, each with its own track's automation, and becomes the range")
{
    Song song;
    const auto other = phazite::insertMidiClip (song.getSong(), song.tracks[1], { at (0.0), at (4.0) }, nullptr);
    REQUIRE (other.isValid());
    phazite::addNote (song.getSong(), other, 48, at (2.0), lasting (1.0), 100);
    const auto pan = phazite::getPan (song.context, song.tracks[1]).getRef();
    phazite::addAutomationPoint (song.context, pan, { BeatPosition::fromBeats (1.0), 0.0f });
    phazite::addAutomationPoint (song.context, pan, { BeatPosition::fromBeats (3.0), 1.0f });
    song.getSong().getUndoManager().clearUndoHistory();
    const auto panAt = [&song, pan] (double beat)
    {
        return phazite::getAutomationValues (song.context, pan, { BeatPosition::fromBeats (beat), BeatPosition::fromBeats (beat) }, 1)
            .front();
    };

    auto& range = getRangeSelection (song.context);
    range.set ({ at (1.0), at (3.0) }, { song.tracks[0], song.tracks[1] });
    REQUIRE (song.registry.invoke ("edit.copy"));
    getSelection (song.context).deselectAll();
    song.playheadAt (16.0);
    phazite::test::UndoCheck check (song.getSong());
    check.command (song.registry, "edit.paste");

    for (const auto track : { 0, 1 })
    {
        CAPTURE (track);
        const auto clips = song.clipsOf (track);
        REQUIRE (clips.size() == 2);
        CHECK (song.rangeOf (clips[1]).getStart().inBeats() == doctest::Approx (16.0));
        CHECK (song.rangeOf (clips[1]).getEnd().inBeats() == doctest::Approx (18.0));
    }

    // The second track's note, a beat into the range, is a beat into its copy.
    const auto notes = song.notesOf (song.clipsOf (1)[1]);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].pitch == 48);
    CHECK (notes[0].start.inBeats() == doctest::Approx (17.0));
    CHECK (song.volumeAt (17.0) == doctest::Approx (song.volumeAt (2.0)));
    CHECK (panAt (17.0) == doctest::Approx (panAt (2.0)));
    CHECK (panAt (16.5) == doctest::Approx (panAt (1.5)));
    CHECK (range.getRange().getStart().inBeats() == doctest::Approx (16.0));
    CHECK (range.getTracks() == std::vector<phazite::TrackID> { song.tracks[0], song.tracks[1] });
    check.walkBackAndForth();
}

TEST_CASE ("Clips copied from a track that has gone since paste on the selected track, and with none selected there is nowhere to paste")
{
    Song song;
    const auto third = phazite::ensureTracks (song.getSong(), 3).back();
    const auto gone = phazite::insertMidiClip (song.getSong(), third, { at (0.0), at (2.0) }, nullptr);
    REQUIRE (gone.isValid());
    song.getSong().getUndoManager().clearUndoHistory();
    auto& selection = getSelection (song.context);
    selection.selectOnly (gone);
    REQUIRE (song.registry.invoke ("edit.copy"));

    phazite::deleteTracks (song.getSong(), { third }, &selection);
    selection.deselectAll();
    CHECK_FALSE (song.isEnabled ("edit.paste"));

    selection.selectOnly (song.tracks[1]);
    REQUIRE (song.isEnabled ("edit.paste"));
    song.playheadAt (4.0);
    phazite::test::UndoCheck check (song.getSong());
    check.command (song.registry, "edit.paste");
    REQUIRE (song.clipsOf (1).size() == 1);
    CHECK (song.rangeOf (song.clipsOf (1).front()).getStart().inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("Notes pasted where they run past the clip's end are pasted whole, and Undo takes them all")
{
    Song song;
    auto& songSelection = song.context.getSongSelection();
    songSelection.setNoteClips ({ song.clip });
    songSelection.selectNotes (phazite::getRefs (phazite::getClipNotes (song.getSong(), song.clip)));
    REQUIRE (song.registry.invoke ("edit.copy"));

    // The clip ends at beat 4: the second note, half a beat after the first and half a beat long, pasted at 3.75 runs a
    // quarter past it.
    song.playheadAt (3.75);
    phazite::test::UndoCheck check (song.getSong());
    check.command (song.registry, "edit.paste");
    const auto notes = song.notesOf (song.clip);
    REQUIRE (notes.size() == 4);
    CHECK (notes[3].start.inBeats() == doctest::Approx (4.25));
    CHECK (songSelection.getNotes().size() == 2);
    CHECK (phazite::test::findIDProblems (song.getSong().getState()).empty());
}

TEST_CASE ("Paste and Duplicate name their step for the notes made where some wouldn't fit before the next clip, and say so")
{
    // The clip, from beat 0 to 4, with the next clip right after it, so that it can't grow; a third note at beat 3.
    Song song;
    phazite::addSelectionCommands (song.registry, getSelection (song.context), getRangeSelection (song.context));
    REQUIRE (phazite::insertMidiClip (song.getSong(), song.tracks[0], { at (4.0), at (8.0) }, nullptr).isValid());
    phazite::addNote (song.getSong(), song.clip, 67, at (3.0), lasting (0.5), 80);
    song.getSong().getUndoManager().clearUndoHistory();
    (void) phazite::takeCommandOutcome();

    auto& songSelection = song.context.getSongSelection();
    songSelection.setNoteClips ({ song.clip });
    songSelection.selectNotes (phazite::getRefs (phazite::getClipNotes (song.getSong(), song.clip)));
    REQUIRE (songSelection.getNotes().size() == 3);

    // Duplicate puts the copies after the last note, from beat 3.5: only the first lands in the clip.
    CHECK (phazite::getCurrentName (*song.registry.find ("edit.duplicate")) == juce::String ("Duplicate 3 Notes"));
    {
        phazite::test::UndoCheck check (song.getSong());
        check.command (song.registry, "edit.duplicate");
    }
    CHECK (song.notesOf (song.clip).size() == 4);
    CHECK (song.describeUndo() == juce::String ("Undo Duplicate Note"));
    CHECK (phazite::takeCommandOutcome().startsWith ("2 notes of 3 weren't made"));
    CHECK (phazite::takeCommandOutcome().isEmpty()); // taken once

    // Paste of the first two at beat 3.75: the second would start at 4.25, in the next clip.
    songSelection.selectNotes (phazite::getRefs ({ song.notesOf (song.clip)[0], song.notesOf (song.clip)[1] }));
    REQUIRE (song.registry.invoke ("edit.copy"));
    song.playheadAt (3.75);
    CHECK (phazite::getCurrentName (*song.registry.find ("edit.paste")) == juce::String ("Paste 2 Notes"));
    {
        phazite::test::UndoCheck check (song.getSong());
        check.command (song.registry, "edit.paste");
    }
    CHECK (song.notesOf (song.clip).size() == 5);
    CHECK (song.describeUndo() == juce::String ("Undo Paste Note"));
    CHECK (phazite::takeCommandOutcome().startsWith ("1 note of 2 wasn't made"));

    // Where all fit, the step keeps its name and nothing is said.
    song.playheadAt (0.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste 2 Notes"));
    CHECK (phazite::takeCommandOutcome().isEmpty());

    // Within a macro, whose step is named for the macro, the step keeps the macro's name.
    REQUIRE (song.registry.addMacro ("test.pasteTwice", "Paste Twice", { "edit.paste" }));
    song.playheadAt (3.75);
    REQUIRE (song.registry.invoke ("test.pasteTwice"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste Twice"));
}
