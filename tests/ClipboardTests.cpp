// SPDX-License-Identifier: AGPL-3.0-or-later
// Cut, Copy and Paste (src/model/arrangement/internal/Clipboard.h; roadmap step 3.7): clips, a time range's clips and
// notes, put in at the playhead, each paste one named undo step that selects what it put in, with the automation under
// clips going back onto their own tracks.
#include "TestHelpers.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/Clips.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::BeatPosition;

te::BeatPosition teBeats (double beat)
{
    return te::BeatPosition::fromBeats (beat);
}

// Two tracks, the clipboard's commands, and on the first track a clip from beat 0 to beat 4 with two notes, under which
// the track's volume rises from 0.2 at beat 1 to 0.8 at beat 3.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        tracks = te::getAudioTracks (*edit);
        setEdit (context, edit.get());
        phazite::addClipboardCommands (registry, context);
        clip = phazite::insertMidiClip (*tracks[0], { teBeats (0.0), teBeats (4.0) }, nullptr);
        phazite::addNote (*clip, 60, teBeats (1.0), te::BeatDuration::fromBeats (0.5), 100);
        phazite::addNote (*clip, 64, teBeats (1.5), te::BeatDuration::fromBeats (0.5), 90);
        volume = phazite::getVolume (context, phazite::getTrackID (*tracks[0])).getRef();
        phazite::addAutomationPoint (context, volume, { BeatPosition::fromBeats (1.0), 0.2f });
        phazite::addAutomationPoint (context, volume, { BeatPosition::fromBeats (3.0), 0.8f });
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    void playheadAt (double beat) { edit->getTransport().setPosition (edit->tempoSequence.toTime (teBeats (beat))); }

    bool isEnabled (const char* id) const { return phazite::CommandRegistry::isEnabled (*registry.find (id)); }

    float volumeAt (double beat) const
    {
        return phazite::getAutomationValues (context, volume, { BeatPosition::fromBeats (beat), BeatPosition::fromBeats (beat) }, 1)
            .front();
    }

    // A track's clips, from the earliest.
    juce::Array<te::Clip*> clipsOf (int track) const
    {
        auto clips = tracks[track]->getClips();
        std::sort (clips.begin(), clips.end(), [] (te::Clip* a, te::Clip* b) { return a->getStartBeat() < b->getStartBeat(); });
        return clips;
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    juce::Array<te::AudioTrack*> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    te::MidiClip* clip = nullptr;
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
    CHECK_FALSE (song.edit->getUndoManager().canUndo());
    CHECK (song.isEnabled ("edit.paste"));

    selection.selectOnly (song.tracks[1]);
    song.playheadAt (8.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste Clip"));

    const auto pasted = song.clipsOf (1);
    REQUIRE (pasted.size() == 1);
    auto* copy = dynamic_cast<te::MidiClip*> (pasted.getFirst());
    REQUIRE (copy != nullptr);
    CHECK (copy->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (copy->getEndBeat().inBeats() == doctest::Approx (12.0));
    CHECK (copy->getSequence().getNumNotes() == 2);
    CHECK_FALSE (copy->isLinked()); // a copy, not an alias
    CHECK (selection.isSelected (copy));
    CHECK_FALSE (selection.isSelected (song.clip));

    // On another track, the automation under it stays where it was.
    CHECK (song.volumeAt (9.0) == doctest::Approx (song.volumeAt (20.0)));

    phazite::undo (*song.edit);
    CHECK (song.tracks[1]->getClips().isEmpty());
}

TEST_CASE ("Cut clips paste back onto their own track with the automation that was under them, in place of what is there")
{
    Song song;
    auto& selection = getSelection (song.context);
    auto* later = phazite::insertMidiClip (*song.tracks[0], { teBeats (10.0), teBeats (14.0) }, nullptr);
    REQUIRE (later != nullptr);
    song.edit->getUndoManager().clearUndoHistory();

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
    CHECK (clips[0]->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (clips[0]->getEndBeat().inBeats() == doctest::Approx (12.0));
    CHECK (clips[1] == later);
    CHECK (later->getStartBeat().inBeats() == doctest::Approx (12.0)); // trimmed: nothing plays twice
    CHECK (song.volumeAt (9.0) == doctest::Approx (0.2f));
    CHECK (song.volumeAt (11.0) == doctest::Approx (0.8f));
    CHECK (song.volumeAt (1.0) == doctest::Approx (0.2f)); // and where it was copied from, still

    phazite::undo (*song.edit);
    CHECK (song.clipsOf (0).size() == 1);
    CHECK (later->getStartBeat().inBeats() == doctest::Approx (10.0));
}

TEST_CASE ("A copied time range pastes its clips cut to its edges, and becomes the range")
{
    Song song;
    auto& range = getRangeSelection (song.context);
    range.set ({ teBeats (1.0), teBeats (3.0) }, { song.tracks[0] });
    REQUIRE (song.registry.invoke ("edit.copy"));
    CHECK (song.clipsOf (0).size() == 1); // the clip itself isn't cut

    song.playheadAt (16.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste Range"));
    const auto clips = song.clipsOf (0);
    REQUIRE (clips.size() == 2);
    CHECK (clips[1]->getStartBeat().inBeats() == doctest::Approx (16.0));
    CHECK (clips[1]->getEndBeat().inBeats() == doctest::Approx (18.0));
    CHECK (range.getRange().getStart().inBeats() == doctest::Approx (16.0));
    CHECK (range.getRange().getEnd().inBeats() == doctest::Approx (18.0));
    CHECK (song.volumeAt (17.0) == doctest::Approx (song.volumeAt (2.0)));
}

TEST_CASE ("Copied notes paste at the playhead in the clip the piano roll shows there, else as far into its first clip")
{
    Song song;
    auto& songSelection = song.context.getSongSelection();
    const auto from = phazite::getClipID (*song.clip);
    auto* other = phazite::insertMidiClip (*song.tracks[1], { teBeats (8.0), teBeats (12.0) }, nullptr);
    REQUIRE (other != nullptr);
    const auto to = phazite::getClipID (*other);
    song.edit->getUndoManager().clearUndoHistory();

    songSelection.setNoteClips ({ from });
    songSelection.selectNotes ({ phazite::getNoteRef (*song.clip, *song.clip->getSequence().getNote (0)),
                                 phazite::getNoteRef (*song.clip, *song.clip->getSequence().getNote (1)) });
    REQUIRE (songSelection.getNotes().size() == 2);
    REQUIRE (song.registry.invoke ("edit.copy"));

    // The piano roll shows the other clip; the playhead isn't in it: a beat into it, as the first note was into its own.
    songSelection.setNoteClips ({ to });
    song.playheadAt (0.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    CHECK (song.describeUndo() == juce::String ("Undo Paste 2 Notes"));
    REQUIRE (other->getSequence().getNumNotes() == 2);
    CHECK (phazite::getNoteStart (*other, *other->getSequence().getNote (0)).inBeats() == doctest::Approx (9.0));
    CHECK (phazite::getNoteStart (*other, *other->getSequence().getNote (1)).inBeats() == doctest::Approx (9.5));
    CHECK (other->getSequence().getNote (1)->getNoteNumber() == 64);
    CHECK (songSelection.getNotes().size() == 2); // the pasted notes

    // With the playhead in it, there.
    song.playheadAt (10.0);
    REQUIRE (song.registry.invoke ("edit.paste"));
    REQUIRE (other->getSequence().getNumNotes() == 4);
    std::vector<double> starts;

    for (auto* note : other->getSequence().getNotes())
        starts.push_back (phazite::getNoteStart (*other, *note).inBeats());

    std::sort (starts.begin(), starts.end());
    CHECK (starts[2] == doctest::Approx (10.0));
    CHECK (starts[3] == doctest::Approx (10.5));
}

TEST_CASE ("Another project empties the clipboard")
{
    Song song;
    getSelection (song.context).selectOnly (song.clip);
    REQUIRE (song.registry.invoke ("edit.copy"));
    CHECK (song.isEnabled ("edit.paste"));

    auto another = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    setEdit (song.context, another.get());
    CHECK_FALSE (song.isEnabled ("edit.paste"));
    setEdit (song.context, song.edit.get());
    CHECK_FALSE (song.isEnabled ("edit.paste"));
}
