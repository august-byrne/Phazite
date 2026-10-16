// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's notes as the piano roll reaches them, by IDs and plain values (src/model/notes/NotesModel.h), and the notes
// selected, by IDs (SongSelection).
#include "TestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

namespace
{
phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

phazite::BeatDuration beats (double length)
{
    return phazite::BeatDuration::fromBeats (length);
}

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

// An open project at 120 BPM with tracks A and B: A with a MIDI clip from beat 4 to 8 holding notes C and E (at its
// beats 0 and 2), B with a MIDI clip from beat 0 to 8 holding a G at beat 1.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");
        setEdit (context, edit.get());

        auto* first = phazite::insertMidiClip (*a, { te::BeatPosition::fromBeats (4.0), te::BeatPosition::fromBeats (8.0) }, nullptr);
        auto* other = phazite::insertMidiClip (*b, { te::BeatPosition(), te::BeatPosition::fromBeats (8.0) }, nullptr);
        REQUIRE (first != nullptr);
        REQUIRE (other != nullptr);
        clip = phazite::getClipID (*first);
        otherClip = phazite::getClipID (*other);
        c = phazite::addNote (context, clip, 60, beat (4.0), beats (1.0), 100);
        e = phazite::addNote (context, clip, 64, beat (6.0), beats (1.0), 90);
        g = phazite::addNote (context, otherClip, 67, beat (1.0), beats (0.5), 80);
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
    phazite::ClipID clip, otherClip;
    phazite::NoteRef c, e, g;
};
} // namespace

TEST_CASE ("The piano roll's clips and notes by ID, in song beats")
{
    Song song;
    auto& context = song.context;
    REQUIRE (song.c.isValid());
    REQUIRE (song.e.isValid());
    CHECK (song.c.clip == song.clip);

    const auto clips = phazite::getNoteClips (context, { song.clip, phazite::ClipID::fromRaw (999999), song.otherClip });
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].id == song.clip);
    CHECK (clips[0].track == idOf (*song.a));
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (clips[0].contentStart.inBeats() == doctest::Approx (4.0));
    CHECK (clips[0].colour == phazite::getTrackColour (*song.a, 0));
    CHECK (clips[1].id == song.otherClip);

    const auto notes = phazite::getNotes (context, { song.clip });
    REQUIRE (notes.size() == 2);
    CHECK (notes[0].getRef() == song.c);
    CHECK (notes[0].start.inBeats() == doctest::Approx (4.0));
    CHECK (notes[1].pitch == 64);
    CHECK (notes[1].velocity == 90);
    CHECK (phazite::getNote (context, song.g)->length.inBeats() == doctest::Approx (0.5));
    CHECK_FALSE (phazite::getNote (context, { song.clip, phazite::NoteID::fromRaw (999999) }).has_value());

    // Other tracks' notes in view are the ghosts: B's G, not A's own.
    const auto ghosts = phazite::getNotesOfOtherTracks (context, { idOf (*song.a) }, { beat (0.0), beat (4.0) });
    REQUIRE (ghosts.size() == 1);
    CHECK (ghosts[0].getRef() == song.g);
    CHECK (phazite::getNotesOfOtherTracks (context, { idOf (*song.a) }, { beat (10.0), beat (12.0) }).empty());

    // A track's clip at the playhead or nearest it; none on a track without MIDI clips.
    CHECK (phazite::getMidiClipNearest (context, idOf (*song.a), beat (20.0)) == song.clip);
    CHECK (phazite::getMidiClipNearest (context, idOf (*song.b), beat (2.0)) == song.otherClip);
    CHECK (phazite::canHoldNotes (context, idOf (*song.a)));
    CHECK_FALSE (phazite::hasAudioClips (context, idOf (*song.a)));

    // A track shown without clips starts one a bar long for the first note drawn there, without selecting it.
    song.edit->ensureNumberOfAudioTracks (3);
    const auto third = idOf (*te::getAudioTracks (*song.edit)[2]);
    CHECK_FALSE (phazite::getMidiClipNearest (context, third, beat (0.0)).has_value());
    const auto started = phazite::addClipForNotes (context, third, beat (8.0));
    REQUIRE (started.isValid());
    CHECK (phazite::getNoteClip (context, started)->range.getEnd().inBeats() == doctest::Approx (12.0));
    CHECK_FALSE (context.getSongSelection().isSelected (started));
}

TEST_CASE ("The piano roll's edits of notes by ID join the step they are in")
{
    Song song;
    auto& context = song.context;
    auto& undo = song.edit->getUndoManager();

    {
        const phazite::UndoStep step (*song.edit, "Edit Notes");
        phazite::moveNote (context, song.c, beat (5.0), 62);
        phazite::setNoteLength (context, song.c, beats (2.0));
        phazite::toggleNotesMuted (context, { song.e });
        phazite::setNoteVelocities (context, { song.c, song.e }, 70);
        phazite::nudgeNotes (context, { song.c, song.e }, beats (-0.5));
    }

    CHECK (undo.getUndoDescription() == juce::String ("Edit Notes"));
    const auto c = phazite::getNote (context, song.c);
    REQUIRE (c.has_value());
    CHECK (c->pitch == 62);
    CHECK (c->start.inBeats() == doctest::Approx (4.5));
    CHECK (c->length.inBeats() == doctest::Approx (2.0));
    CHECK (c->velocity == 70);
    CHECK (phazite::getNote (context, song.e)->muted);

    // A step of its own for the velocity wheel.
    phazite::changeNoteVelocities (context, { song.c }, 5);
    CHECK (phazite::getNote (context, song.c)->velocity == 75);
    CHECK (undo.getUndoDescription() == juce::String ("Change Velocity"));

    // Replaced by a transform's result: the notes keep their IDs; then deleted.
    auto data = phazite::getNotes (context, { song.clip });
    for (auto& note : data)
        note.pitch += 12;

    const auto replaced = phazite::replaceNotes (context, { song.c, song.e }, data, "Transpose Notes");
    CHECK (undo.getUndoDescription() == juce::String ("Transpose Notes"));
    REQUIRE (replaced.size() == 2);
    CHECK (std::find (replaced.begin(), replaced.end(), song.c) != replaced.end());
    CHECK (phazite::getNote (context, song.c)->pitch == 74);

    phazite::deleteNotes (context, { song.c });
    CHECK_FALSE (phazite::getNote (context, song.c).has_value());

    // A note added by ID.
    const auto added = phazite::addNote (context, song.clip, 72, beat (7.0), beats (0.25), 100);
    REQUIRE (added.isValid());
    CHECK (phazite::getNote (context, added)->start.inBeats() == doctest::Approx (7.0));
}

TEST_CASE ("A track's key and drum rows by ID")
{
    Song song;
    auto& context = song.context;
    auto& undo = song.edit->getUndoManager();
    const auto a = idOf (*song.a);

    CHECK_FALSE (phazite::getTrackKey (context, a).has_value());
    phazite::chooseTrackKey (context, a, phazite::Key { 9, phazite::Scale::minor });
    CHECK (phazite::getTrackKey (context, a) == phazite::Key { 9, phazite::Scale::minor });
    CHECK (undo.getUndoDescription() == juce::String ("Set Key"));

    phazite::useKeyOnEveryTrack (context, phazite::Key { 2, phazite::Scale::dorian });
    CHECK (phazite::getTrackKey (context, idOf (*song.b)) == phazite::Key { 2, phazite::Scale::dorian });
    CHECK (undo.getUndoDescription() == juce::String ("Use This Key on Every Track"));

    // Drum mode comes with the drum sampler; its rows' order and hidden rows are view state.
    CHECK_FALSE (phazite::isDrumTrack (context, a));
    phazite::chooseInstrument (context, a, song.find ("phaziteDrums"));
    CHECK (phazite::isDrumTrack (context, a));
    REQUIRE (phazite::getSamplerSounds (context, a).has_value());
    CHECK (phazite::getSamplerSounds (context, a)->size() == 16);
    CHECK (phazite::getNoteNumbersPlayed (context, { song.clip, song.otherClip }) == std::vector { 60, 64, 67 });

    undo.clearUndoHistory();
    phazite::setDrumRowOrder (context, a, { 38, 36 });
    phazite::hideDrumRow (context, a, 42);
    CHECK (phazite::getDrumRowOrder (context, a) == std::vector { 38, 36 });
    CHECK (phazite::getHiddenDrumRows (context, a) == std::vector { 42 });
    phazite::showAllDrumRows (context, a);
    CHECK (phazite::getHiddenDrumRows (context, a).empty());
    phazite::setDrumTrack (context, a, false);
    CHECK_FALSE (phazite::isDrumTrack (context, a));
    CHECK_FALSE (undo.canUndo());
}

TEST_CASE ("Notes are selected by ID, of the clips an editor shows, and nothing else is selected with them")
{
    Song song;
    auto& selection = song.context.getSongSelection();
    CHECK (selection.getNotes().empty());

    selection.setNoteClips ({ song.clip });
    selection.selectNotes ({ song.c });
    CHECK (selection.getNotes() == std::vector { song.c });
    CHECK (selection.isSelected (song.c));
    CHECK_FALSE (selection.isSelected (song.e));

    selection.selectNotes ({ song.e }, true);
    CHECK (selection.getNotes().size() == 2);
    selection.selectNotes ({ song.g }); // another clip's: not among those shown
    CHECK (selection.getNotes().empty());

    // The model's note commands act on them.
    selection.selectNotes ({ song.c, song.e });
    CHECK (phazite::getSelectedNotes (getSelection (song.context)).size() == 2);

    // Selecting a clip deselects them; a deleted note is no longer selected.
    selection.selectOnly (song.clip);
    CHECK (selection.getNotes().empty());
    selection.selectNotes ({ song.c, song.e });
    phazite::deleteNotes (song.context, { song.e });
    CHECK (selection.getNotes() == std::vector { song.c });

    // Other clips shown: the notes of those before can't be selected.
    selection.setNoteClips ({ song.otherClip });
    CHECK (selection.getNotes().empty());
    selection.selectNotes ({ song.g });
    CHECK (selection.getNotes() == std::vector { song.g });

    // A shown clip deleted takes its notes out of the selection.
    phazite::deleteClips ({ te::findClipForID (*song.edit, phazite::toTracktion (song.otherClip)) }, false);
    CHECK (selection.getNotes().empty());
}
