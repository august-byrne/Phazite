// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/KeyDispatcher.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

phazite::BeatDuration length (double b)
{
    return phazite::BeatDuration::fromBeats (b);
}

// A note's values, in no clip yet.
phazite::NoteData noteData (int pitch, double start, double noteLength, int velocity)
{
    return { .pitch = pitch,
             .start = phazite::BeatPosition::fromBeats (start),
             .length = phazite::BeatDuration::fromBeats (noteLength),
             .velocity = velocity };
}

// A note of the song's, which must be there.
phazite::NoteData noteOf (const phazite::Song& song, const phazite::NoteRef& note)
{
    const auto data = phazite::getNoteData (song, note);
    REQUIRE (data.has_value());
    return *data;
}

// A song with a MIDI clip from beat 4 to 8 (one bar, 4/4), the note selection for it, and the selection
// and note commands, with a grid of a quarter beat.
struct Notes
{
    Notes()
    {
        track = phazite::ensureTracks (song, 1).front();
        clip = phazite::insertMidiClip (song, track, { beats (4), beats (8) }, nullptr);
        REQUIRE (clip.isValid());
        selection.setSong (&song);
        selection.setNoteClips ({ clip });
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addNoteCommands (registry, selection, [] { return 0.25; });
        song.getUndoManager().clearUndoHistory();
    }

    ~Notes() { selection.deselectAll(); }

    phazite::NoteRef add (int pitch, double start, double noteLength, int velocity = 100)
    {
        const auto note = phazite::addNote (song, clip, pitch, beats (start), length (noteLength), velocity);
        REQUIRE (note.isValid());
        return note;
    }

    void select (const std::vector<phazite::NoteRef>& notes) { selection.selectNotes (notes); }

    phazite::NoteData get (const phazite::NoteRef& note) const { return noteOf (song, note); }
    double startOf (const phazite::NoteRef& note) const { return get (note).start.inBeats(); }
    phazite::BeatRange rangeOf (phazite::ClipID id) const { return phazite::getClipRange (song, id); }
    size_t numNotesIn (phazite::ClipID id) const { return phazite::getClipNotes (song, id).size(); }

    phazite::Song::Ptr songPtr = phazite::createSong();
    phazite::Song& song = *songPtr;
    phazite::TrackID track;
    phazite::ClipID clip;
    phazite::Selection selection;
    phazite::RangeSelection range { selection };
    phazite::CommandRegistry registry;
};

// An edit of the model's that joins its caller's step, made as a command makes it: one named undo step, checked exact.
void step (phazite::test::UndoCheck& check, phazite::Song& song, const juce::String& name, const std::function<void()>& edit)
{
    check.check (name,
                 [&]
                 {
                     const phazite::UndoStep undoStep (song, name);
                     edit();
                 });
}
} // namespace

TEST_CASE ("Notes are placed in song beats, and their clip grows to hold them")
{
    Notes n;
    phazite::test::UndoCheck check (n.song);
    phazite::NoteRef note;
    step (check, n.song, "Add Note", [&] { note = n.add (60, 5.0, 1.0); });
    CHECK (n.startOf (note) == doctest::Approx (5.0));
    const auto inClip = (n.get (note).start - phazite::getContentStart (phazite::findClipState (n.song, n.clip))).inBeats();
    CHECK (inClip == doctest::Approx (1.0)); // in the clip, from its content's start

    // A note can't start before its clip, and one past its end makes it grow to the end of that bar.
    phazite::NoteRef early;
    step (check, n.song, "Add Note", [&] { early = n.add (62, 2.0, 1.0); });
    CHECK (n.startOf (early) == doctest::Approx (4.0));

    step (check, n.song, "Add Note", [&] { n.add (64, 9.0, 0.5); });
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (12.0));

    step (check, n.song, "Move Note", [&] { phazite::moveNote (n.song, note, beats (13.5), 72); });
    CHECK (n.startOf (note) == doctest::Approx (13.5));
    CHECK (n.get (note).pitch == 72);
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (16.0));

    step (check, n.song, "Set Note Length", [&] { phazite::setNoteLength (n.song, note, length (0.0)); });
    CHECK (n.get (note).length.inBeats() == doctest::Approx (phazite::getMinimumNoteLength().inBeats()));
    check.walkBackAndForth();
}

TEST_CASE ("Transposing and nudging keep the notes together, within their limits")
{
    Notes n;
    const auto low = n.add (2, 4.5, 1.0);
    const auto high = n.add (60, 5.0, 1.0);
    const std::vector<phazite::NoteRef> both { low, high };
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);

    // The lowest note can only go down 2, so the chord goes down 2.
    step (check, n.song, "Transpose Notes", [&] { phazite::transposeNotes (n.song, both, -12); });
    CHECK (n.get (low).pitch == 0);
    CHECK (n.get (high).pitch == 58);

    // The earliest can only go back half a beat, to the clip's start.
    step (check, n.song, "Nudge Notes", [&] { phazite::nudgeNotes (n.song, both, length (-2.0)); });
    CHECK (n.startOf (low) == doctest::Approx (4.0));
    CHECK (n.startOf (high) == doctest::Approx (4.5));

    step (check, n.song, "Lengthen Notes", [&] { phazite::changeNoteLengths (n.song, both, length (0.5)); });
    CHECK (n.get (high).length.inBeats() == doctest::Approx (1.5));

    step (check, n.song, "Mute Notes", [&] { phazite::toggleNotesMuted (n.song, both); });
    CHECK (phazite::areAllNotesMuted (n.song, both));
    step (check, n.song, "Unmute Notes", [&] { phazite::toggleNotesMuted (n.song, both); });
    CHECK_FALSE (n.get (low).muted);

    step (check, n.song, "Set Velocity", [&] { phazite::setNoteVelocities (n.song, both, 200); });
    CHECK (n.get (high).velocity == 127);
    check.walkBackAndForth();
}

TEST_CASE ("Duplicated notes go right after the selection")
{
    Notes n;
    const auto a = n.add (60, 4.0, 1.0);
    const auto b = n.add (64, 5.0, 0.5);
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);
    std::vector<phazite::NoteRef> copies;
    step (check, n.song, "Duplicate Notes", [&] { copies = phazite::duplicateNotes (n.song, { a, b }); });

    REQUIRE (copies.size() == 2);
    CHECK (n.startOf (copies[0]) == doctest::Approx (5.5));
    CHECK (n.startOf (copies[1]) == doctest::Approx (6.5));
    CHECK (n.get (copies[1]).pitch == 64);
    CHECK (n.numNotesIn (n.clip) == 4);
}

TEST_CASE ("A clip grows into the empty space after it only: never over the next clip on its track")
{
    Notes n; // its clip is beats 4 to 8, on the song's only track
    const auto track = phazite::getClipTrack (n.song, n.clip);
    const auto addClip = [&n, track] (double start, double end)
    {
        const auto added = phazite::insertMidiClip (n.song, track, { beats (start), beats (end) }, nullptr);
        REQUIRE (added.isValid());
        return added;
    };

    SUBCASE ("A clip right after it: it doesn't grow, and the note stays in it without playing")
    {
        const auto next = addClip (8.0, 12.0);
        phazite::test::UndoCheck check (n.song);
        phazite::NoteRef note;
        step (check, n.song, "Add Note", [&] { note = n.add (60, 8.5, 1.0); });
        CHECK (n.startOf (note) == doctest::Approx (8.5));
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (8.0));
        CHECK (n.rangeOf (next).getStart().inBeats() == doctest::Approx (8.0));
        CHECK (n.numNotesIn (next) == 0);
    }

    SUBCASE ("A clip before the bar's end: it grows to meet it, not past it")
    {
        const auto next = addClip (10.0, 14.0);
        phazite::test::UndoCheck check (n.song);
        phazite::NoteRef note;
        step (check, n.song, "Add Note", [&] { note = n.add (60, 9.0, 0.5); });
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (10.0)); // not 12, where the bar ends
        step (check, n.song, "Move Note", [&] { phazite::moveNote (n.song, note, beats (13.0), 60); });
        step (check, n.song, "Set Note Length", [&] { phazite::setNoteLength (n.song, note, length (4.0)); });
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (10.0));
        CHECK (n.rangeOf (next).getStart().inBeats() == doctest::Approx (10.0));
        CHECK (n.rangeOf (next).getEnd().inBeats() == doctest::Approx (14.0));
        check.walkBackAndForth();
    }

    SUBCASE ("A clip that already reaches past its end: it doesn't grow")
    {
        addClip (6.0, 10.0);
        phazite::test::UndoCheck check (n.song);
        step (check, n.song, "Add Note", [&] { n.add (60, 8.5, 0.5); });
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (8.0));
    }

    SUBCASE ("Clips on other tracks and sections don't count")
    {
        const auto other = phazite::insertTrack (n.song, n.track, nullptr);
        REQUIRE (other.isValid());
        REQUIRE (phazite::insertMidiClip (n.song, other, { beats (8.0), beats (12.0) }, nullptr).isValid());
        phazite::SectionInfo drop;
        drop.name = "Drop";
        REQUIRE (phazite::insertSection (n.song, phazite::BeatPosition::fromBeats (8.0), drop).isValid());
        phazite::test::UndoCheck check (n.song);
        step (check, n.song, "Add Note", [&] { n.add (60, 9.0, 0.5); });
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (12.0));
    }
}

TEST_CASE ("Duplicating notes past their clip grows it only up to the next clip, in one undo step")
{
    Notes n;
    const auto next = phazite::insertMidiClip (n.song, phazite::getClipTrack (n.song, n.clip), { beats (10.0), beats (14.0) }, nullptr);
    REQUIRE (next.isValid());
    const auto a = n.add (60, 6.0, 1.0);
    const auto b = n.add (64, 7.0, 0.5);
    n.song.getUndoManager().clearUndoHistory();
    n.select ({ a, b });
    phazite::test::UndoCheck check (n.song);

    check.command (n.registry, "edit.duplicate");
    CHECK (n.song.getUndoManager().getUndoDescription() == juce::String ("Duplicate 2 Notes"));

    const auto copies = phazite::getSelectedNotes (n.selection);
    REQUIRE (copies.size() == 2);
    CHECK (n.startOf (copies[0]) == doctest::Approx (7.5));
    CHECK (n.startOf (copies[1]) == doctest::Approx (8.5));
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (10.0)); // up to the next clip, not to beat 12
    CHECK (n.rangeOf (next).getStart().inBeats() == doctest::Approx (10.0));

    n.selection.deselectAll(); // the copies go with the undo
    check.stepBackOrForth ("Undo", [&n] { phazite::undo (n.song); });
    CHECK (n.numNotesIn (n.clip) == 2);
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (8.0));
}

TEST_CASE ("Quantize moves notes toward the grid, with strength and swing")
{
    using phazite::NoteData;
    const std::vector<NoteData> notes { noteData (60, 0.1, 0.3, 100), noteData (62, 0.6, 0.3, 100) };

    phazite::QuantizeSettings settings;
    settings.gridBeats = 0.5;
    auto result = phazite::quantize (notes, settings);
    CHECK (result[0].start.inBeats() == doctest::Approx (0.0));
    CHECK (result[1].start.inBeats() == doctest::Approx (0.5));
    CHECK (result[0].length.inBeats() == doctest::Approx (0.3)); // only the starts, by default

    settings.strength = 0.5f;
    result = phazite::quantize (notes, settings);
    CHECK (result[0].start.inBeats() == doctest::Approx (0.05));
    CHECK (result[1].start.inBeats() == doctest::Approx (0.55));

    // At full swing, every second line is a third of a step late: a triplet feel.
    settings.strength = 1.0f;
    settings.swing = 1.0f;
    result = phazite::quantize (notes, settings);
    CHECK (result[0].start.inBeats() == doctest::Approx (0.0));
    CHECK (result[1].start.inBeats() == doctest::Approx (0.5 + 0.5 / 3.0));

    settings.swing = 0.0f;
    settings.ends = true;
    result = phazite::quantize (notes, settings);
    CHECK (result[0].getEnd().inBeats() == doctest::Approx (0.5));
}

TEST_CASE ("Legato and chop")
{
    using phazite::NoteData;
    std::vector<NoteData> notes { noteData (60, 0.0, 0.25, 100), noteData (64, 0.0, 0.5, 100), // a chord with the first
                                  noteData (67, 1.0, 0.25, 100) };

    const auto connected = phazite::legato (notes);
    CHECK (connected[0].length.inBeats() == doctest::Approx (1.0));
    CHECK (connected[1].length.inBeats() == doctest::Approx (1.0));
    CHECK (connected[2].length.inBeats() == doctest::Approx (0.25)); // nothing after it

    CHECK (phazite::legato (notes, 0.25)[0].length.inBeats() == doctest::Approx (0.75));

    const auto pieces = phazite::chop ({ noteData (60, 2.0, 1.1, 90) }, 0.25);
    REQUIRE (pieces.size() == 5);
    CHECK (pieces[1].start.inBeats() == doctest::Approx (2.25));
    CHECK (pieces[4].length.inBeats() == doctest::Approx (0.1));
    CHECK (pieces[4].velocity == 90);
}

TEST_CASE ("Chop and Arpeggiate keep all of a note too long for their most pieces, and Quantize takes the nearest swung line")
{
    // Past 256 pieces, the rest of a long note was deleted; and Quantize took the nearest line before swing, then swung it
    // away from a nearer one (known bug 52).
    using phazite::NoteData;
    const auto pieces = phazite::chop ({ noteData (60, 4.0, 100.0, 90) }, 0.25);
    REQUIRE (pieces.size() == 256);
    CHECK (pieces[254].length.inBeats() == doctest::Approx (0.25));
    CHECK (pieces.back().start.inBeats() == doctest::Approx (4.0 + 255 * 0.25));
    CHECK (pieces.back().getEnd().inBeats() == doctest::Approx (104.0));

    const std::vector<NoteData> chord { noteData (60, 0.0, 300.0, 100), noteData (64, 0.0, 300.0, 100), noteData (67, 0.0, 300.0, 100) };
    const auto arpeggio = phazite::arpeggiate (chord, { .stepBeats = 0.25 });
    REQUIRE (arpeggio.size() == 256 * 3);
    CHECK (arpeggio.back().getEnd().inBeats() == doctest::Approx (300.0));

    // A grid of half a beat at full swing: lines at 0, 2/3, 1 and 5/3. A note at 0.3 is nearest the one at 0, not the swung
    // one the unswung line at a half becomes.
    phazite::QuantizeSettings settings;
    settings.gridBeats = 0.5;
    settings.swing = 1.0f;
    const auto quantized = phazite::quantize ({ noteData (60, 0.3, 0.25, 100), noteData (62, 0.9, 0.25, 100) }, settings);
    CHECK (quantized[0].start.inBeats() == doctest::Approx (0.0));
    CHECK (quantized[1].start.inBeats() == doctest::Approx (1.0));
}

TEST_CASE ("Quantize counts the grid from the piano roll's bar 1, as its snapping does")
{
    // A clip that starts a third of a beat into the song: the grid's lines are a beat apart from there.
    phazite::QuantizeSettings settings;
    settings.gridBeats = 1.0;
    settings.origin = beats (1.0 / 3.0);

    const auto result = phazite::quantize ({ noteData (60, 1.4, 0.5, 100), noteData (62, 2.2, 0.5, 100) }, settings);
    CHECK (result[0].start.inBeats() == doctest::Approx (1.0 + 1.0 / 3.0));
    CHECK (result[1].start.inBeats() == doctest::Approx (2.0 + 1.0 / 3.0));
}

TEST_CASE ("Strum starts a chord's notes one after another and keeps their ends; single notes stay")
{
    using phazite::NoteData;
    const std::vector<NoteData> notes { noteData (64, 0.0, 1.0, 100), noteData (60, 0.0, 1.0, 100), noteData (67, 0.0, 1.0, 100),
                                        noteData (72, 2.0, 1.0, 100) };

    auto result = phazite::strum (notes, { .stepBeats = 0.125 });
    CHECK (result[1].start.inBeats() == doctest::Approx (0.0)); // the lowest first
    CHECK (result[0].start.inBeats() == doctest::Approx (0.125));
    CHECK (result[2].start.inBeats() == doctest::Approx (0.25));
    CHECK (result[2].getEnd().inBeats() == doctest::Approx (1.0)); // its end stays
    CHECK (result[3].start.inBeats() == doctest::Approx (2.0));    // alone: not a chord

    result = phazite::strum (notes, { .stepBeats = 0.125, .down = true });
    CHECK (result[2].start.inBeats() == doctest::Approx (0.0)); // the highest first
    CHECK (result[1].start.inBeats() == doctest::Approx (0.25));

    // A step longer than a note leaves it the shortest a note can be, ending where it did.
    result = phazite::strum ({ noteData (60, 0.0, 0.25, 100), noteData (64, 0.0, 0.25, 100) }, { .stepBeats = 1.0 });
    CHECK (result[1].length == phazite::getMinimumNoteLength());
    CHECK (result[1].getEnd().inBeats() == doctest::Approx (0.25));
}

TEST_CASE ("Humanize moves starts and velocities within its limits, the same way each time")
{
    using phazite::NoteData;
    std::vector<NoteData> notes;

    for (int i = 0; i < 32; ++i)
        notes.push_back (noteData (60, 1.0 + i, 0.5, 100));

    const phazite::HumanizeSettings settings { .timingBeats = 0.1, .velocity = 20, .seed = 7 };
    const auto result = phazite::humanize (notes, settings);
    auto moved = false;

    for (size_t i = 0; i < notes.size(); ++i)
    {
        CHECK (std::abs (result[i].start.inBeats() - notes[i].start.inBeats()) <= 0.1 + 1.0e-9);
        CHECK (std::abs (result[i].velocity - notes[i].velocity) <= 20);
        CHECK (result[i].length.inBeats() == doctest::Approx (0.5));
        moved = moved || std::abs (result[i].start.inBeats() - notes[i].start.inBeats()) > 1.0e-6;
    }

    CHECK (moved);

    // The same settings, the same result: what the piano roll previews is what it applies.
    const auto again = phazite::humanize (notes, settings);

    for (size_t i = 0; i < notes.size(); ++i)
    {
        CHECK (again[i].start.inBeats() == doctest::Approx (result[i].start.inBeats()));
        CHECK (again[i].velocity == result[i].velocity);
    }

    // No timing leaves the starts; changing the velocity's amount keeps what the timing does.
    const auto velocityOnly = phazite::humanize (notes, { .timingBeats = 0.0, .velocity = 20, .seed = 7 });
    const auto timingOnly = phazite::humanize (notes, { .timingBeats = 0.1, .velocity = 0, .seed = 7 });

    for (size_t i = 0; i < notes.size(); ++i)
    {
        CHECK (velocityOnly[i].start.inBeats() == doctest::Approx (notes[i].start.inBeats()));
        CHECK (velocityOnly[i].velocity == result[i].velocity);
        CHECK (timingOnly[i].start.inBeats() == doctest::Approx (result[i].start.inBeats()));
        CHECK (timingOnly[i].velocity == notes[i].velocity);
    }
}

TEST_CASE ("Arpeggiate plays a chord's notes one after another for as long as it lasts")
{
    using phazite::NoteData;

    // A C major chord a beat long, as loud as each note was, and a single note after it.
    const std::vector<NoteData> notes { noteData (64, 0.0, 1.0, 80), noteData (60, 0.0, 1.0, 100), noteData (67, 0.0, 0.5, 60),
                                        noteData (72, 2.0, 1.0, 90) };

    const auto pitchesOf = [] (const std::vector<NoteData>& result)
    {
        std::vector<int> pitches;

        for (const auto& note : result)
            if (note.start.inBeats() < 1.5)
                pitches.push_back (note.pitch);

        return pitches;
    };

    auto result = phazite::arpeggiate (notes, { .stepBeats = 0.25 });
    CHECK (pitchesOf (result) == std::vector { 60, 64, 67, 60 }); // up, until the longest note's end
    REQUIRE (result.size() == 5);
    CHECK (result[1].start.inBeats() == doctest::Approx (0.25));
    CHECK (result[1].length.inBeats() == doctest::Approx (0.25));
    CHECK (result[1].velocity == 80);
    CHECK (result[4].pitch == 72); // alone: not a chord
    CHECK (result[4].start.inBeats() == doctest::Approx (2.0));

    CHECK (pitchesOf (phazite::arpeggiate (notes, { .stepBeats = 0.25, .order = phazite::ArpeggioOrder::down }))
           == std::vector { 67, 64, 60, 67 });
    CHECK (pitchesOf (phazite::arpeggiate (notes, { .stepBeats = 0.125, .order = phazite::ArpeggioOrder::upAndDown }))
           == std::vector { 60, 64, 67, 64, 60, 64, 67, 64 });

    // The gate shortens each note within its step.
    result = phazite::arpeggiate (notes, { .stepBeats = 0.25, .gate = 0.5f });
    CHECK (result[0].length.inBeats() == doctest::Approx (0.125));
}

TEST_CASE ("Lengths read as the grid names its steps, and Legato's name shows its gap")
{
    CHECK (phazite::describeNoteLength (length (0.25)) == "1/4 beat");
    CHECK (phazite::describeNoteLength (length (0.75)) == "3/4 beat");
    CHECK (phazite::describeNoteLength (length (1.0 / 3.0)) == "1/3 beat");
    CHECK (phazite::describeNoteLength (length (1.0)) == "1 beat");
    CHECK (phazite::describeNoteLength (length (4.0)) == "4 beats");
    CHECK (phazite::describeNoteLength (length (1.5)) == "1.5 beats");
    CHECK (phazite::describeNoteLength (length (0.37)) == "0.37 beat");

    CHECK (phazite::describeLegato (0.0) == "Legato");
    CHECK (phazite::describeLegato (0.0625) == "Legato (1/16 beat gap)");
}

TEST_CASE ("Legato leaves the gap the piano roll gave it last, and says so in its name")
{
    Notes n;
    phazite::CommandRegistry registry; // with a gap, beside the commands without one that Notes has
    auto gap = 0.25;
    phazite::addNoteCommands (registry, n.selection, [] { return 0.25; }, [&gap] { return gap; });

    const auto first = n.add (60, 4.0, 0.25), second = n.add (62, 5.0, 0.25);
    n.select ({ first, second });

    const auto* legatoCommand = registry.find ("note.legato");
    REQUIRE (legatoCommand != nullptr);
    CHECK (phazite::getCurrentName (*legatoCommand) == "Legato (1/4 beat gap)");

    REQUIRE (registry.invoke ("note.legato"));
    REQUIRE (n.numNotesIn (n.clip) == 2);
    CHECK (phazite::getClipNotes (n.song, n.clip)[0].length.inBeats() == doctest::Approx (0.75));
    CHECK (phazite::describeUndo (&n.song) == juce::String ("Undo Legato (1/4 beat gap)"));

    gap = 0.0;
    CHECK (phazite::getCurrentName (*legatoCommand) == "Legato");
}

TEST_CASE ("Keys: shading, snapping and transposing in the key")
{
    const phazite::Key cMajor { 0, phazite::Scale::major };
    CHECK (phazite::isInKey (60, cMajor));
    CHECK_FALSE (phazite::isInKey (61, cMajor));
    CHECK (phazite::snapToKey (61, cMajor) == 60); // the lower one when both are as near
    CHECK (phazite::snapToKey (66, cMajor) == 65);
    CHECK (phazite::describeKey (cMajor) == juce::String ("C major"));

    const phazite::Key aMinor { 9, phazite::Scale::minor };
    CHECK (phazite::isInKey (60, aMinor));
    CHECK_FALSE (phazite::isInKey (61, aMinor));

    using phazite::NoteData;
    const std::vector<NoteData> notes { noteData (64, 0.0, 1.0, 100) };

    // E up two steps of C major is G; up a semitone, F; down 3 steps, B below.
    CHECK (phazite::transpose (notes, 2, cMajor)[0].pitch == 67);
    CHECK (phazite::transpose (notes, 1, std::nullopt)[0].pitch == 65);
    CHECK (phazite::transpose (notes, -3, cMajor)[0].pitch == 59);
    CHECK (phazite::transpose (notes, 7, cMajor)[0].pitch == 76); // an octave
    CHECK (phazite::transpose ({ noteData (125, 0.0, 1.0, 100) }, 12, std::nullopt)[0].pitch == 127);
}

TEST_CASE ("A track has no key until one is chosen, and new tracks get the key chosen last")
{
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    CHECK_FALSE (phazite::getTrackKey (*song, track).has_value());

    song->getUndoManager().beginNewTransaction();
    phazite::setTrackKey (*song, track, phazite::Key { 9, phazite::Scale::minor });
    const auto key = phazite::getTrackKey (*song, track);
    REQUIRE (key.has_value());
    CHECK (phazite::describeKey (*key) == juce::String ("A minor"));
    CHECK (phazite::findTrackState (*song, track)[phazite::schema::keyScale].toString() == juce::String ("minor")); // by name

    // A new track gets it.
    const auto added = phazite::insertTrack (*song, track, nullptr);
    REQUIRE (added.isValid());
    REQUIRE (phazite::getTrackKey (*song, added).has_value());
    CHECK (phazite::getTrackKey (*song, added)->root == 9);
    CHECK (phazite::getTrackKey (*song, added)->scale == phazite::Scale::minor);

    // Turning it off is an undo step.
    song->getUndoManager().beginNewTransaction();
    phazite::setTrackKey (*song, track, std::nullopt);
    CHECK_FALSE (phazite::getTrackKey (*song, track).has_value());
    phazite::undo (*song);
    CHECK (phazite::getTrackKey (*song, track).has_value());
}

TEST_CASE ("Chord stamps and drum rows")
{
    CHECK (phazite::getChordPitches (60, phazite::ChordType::major) == std::vector<int> { 60, 64, 67 });
    CHECK (phazite::getChordPitches (57, phazite::ChordType::minorSeventh) == std::vector<int> { 57, 60, 64, 67 });
    CHECK (phazite::getChordPitches (125, phazite::ChordType::major) == std::vector<int> { 125 }); // the rest are out of range
    CHECK_FALSE (phazite::getChordShapes().empty());

    CHECK (phazite::getDrumName (36) == juce::String ("Kick"));
    CHECK (phazite::getDrumName (42) == juce::String ("Closed Hat"));
    CHECK (phazite::getDrumName (56) == juce::String ("Cowbell"));

    Notes n;
    const auto track = n.track;
    CHECK_FALSE (phazite::isDrumTrack (n.song, track)); // no sampler
    phazite::setDrumTrack (n.song, track, true);
    CHECK (phazite::isDrumTrack (n.song, track));

    // What the piano roll's rows come from (DrumRowTests): no sampler, so no sounds of its own, and the notes the clip
    // plays.
    n.add (70, 4.0, 0.25);
    CHECK_FALSE (phazite::getSamplerSounds (n.song, track).has_value());
    CHECK (phazite::getNoteNumbersPlayed (n.song, { n.clip }) == std::vector<int> { 70 });

    // The track keeps the rows' order and the hidden rows, as view settings.
    n.song.getUndoManager().clearUndoHistory();
    phazite::setDrumRowOrder (n.song, track, { 70, 36 });
    CHECK (phazite::getDrumRowOrder (n.song, track) == std::vector<int> { 70, 36 });
    phazite::hideDrumRow (n.song, track, 37);
    phazite::hideDrumRow (n.song, track, 37);
    CHECK (phazite::getHiddenDrumRows (n.song, track) == std::vector<int> { 37 }); // once
    CHECK_FALSE (n.song.getUndoManager().canUndo());

    phazite::showAllDrumRows (n.song, track);
    CHECK (phazite::getHiddenDrumRows (n.song, track).empty());
}

TEST_CASE ("The note commands act on the selected notes, and take the arrow keys while notes are selected")
{
    Notes n;
    const auto a = n.add (60, 4.0, 1.0);
    const auto b = n.add (64, 5.0, 1.0);
    auto& undoManager = n.song.getUndoManager();
    undoManager.clearUndoHistory();
    phazite::test::UndoCheck check (n.song);

    CHECK_FALSE (n.registry.find ("note.transposeUp")->isEnabled());
    n.select ({ a, b });
    CHECK (phazite::getSelectedNotes (n.selection).size() == 2);

    check.command (n.registry, "note.octaveUp");
    CHECK (n.get (a).pitch == 72);
    CHECK (undoManager.getUndoDescription() == juce::String ("Octave Up"));

    check.command (n.registry, "note.lengthen");
    CHECK (n.get (b).length.inBeats() == doctest::Approx (1.25)); // a step of the grid

    // Legato replaces the notes and selects the new ones.
    check.command (n.registry, "note.legato");
    auto selected = phazite::getSelectedNotes (n.selection);
    REQUIRE (selected.size() == 2);
    CHECK (n.get (selected[0]).length.inBeats() == doctest::Approx (1.0));

    check.command (n.registry, "note.chop");
    CHECK (n.numNotesIn (n.clip) == 4 + 5); // 1 beat in 4 pieces, 1.25 beats in 5
    CHECK (phazite::getSelectedNotes (n.selection).size() == 9);

    // The up arrow runs the note command while notes are selected, and nothing once none are. (ShortcutTests checks
    // that a note command wins over a track command on the same key.)
    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["note.transposeUp"] = { "cursor up" };
    phazite::ui::CommandBridge commands { n.registry };
    commands.setKeymap (keymap);
    const phazite::ui::KeyDispatcher keys { commands, phazite::ui::getUsLayout() };
    CHECK (keys.findCommandForKey (juce::KeyPress (juce::KeyPress::upKey))->id == juce::String ("note.transposeUp"));

    n.selection.deselectAll();
    CHECK (keys.findCommandForKey (juce::KeyPress (juce::KeyPress::upKey)) == nullptr);
}

TEST_CASE ("Replacing the selected notes selects the new ones")
{
    Notes n;
    const auto a = n.add (60, 4.0, 1.0);
    const auto b = n.add (64, 5.0, 1.0);
    const auto octaveUp = [] (std::vector<phazite::NoteData> notes)
    {
        for (auto& data : notes)
            data.pitch += 12;

        return notes;
    };

    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);

    // With no notes selected, nothing changes.
    CHECK_FALSE (phazite::hasSelectedNotes (n.selection));
    check.check ("Nothing", [&] { CHECK (phazite::replaceSelectedNotes (n.selection, octaveUp).empty()); });
    CHECK (n.numNotesIn (n.clip) == 2);

    n.select ({ a, b });
    CHECK (phazite::hasSelectedNotes (n.selection));
    std::vector<phazite::NoteRef> added;
    step (check, n.song, "Octave Up", [&] { added = phazite::replaceSelectedNotes (n.selection, octaveUp); });
    REQUIRE (added.size() == 2);
    CHECK (n.numNotesIn (n.clip) == 2); // replaced, not added to

    std::vector<int> pitches;

    for (const auto& selected : phazite::getSelectedNotes (n.selection))
        pitches.push_back (n.get (selected).pitch);

    std::sort (pitches.begin(), pitches.end());
    CHECK (pitches == std::vector<int> { 72, 76 });
}

TEST_CASE ("Every track can get the same key")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    REQUIRE (tracks.size() == 3);

    phazite::setTrackKeys (*song, tracks, phazite::Key { 2, phazite::Scale::minor });

    for (const auto track : tracks)
    {
        const auto key = phazite::getTrackKey (*song, track);
        REQUIRE (key.has_value());
        CHECK (phazite::describeKey (*key) == juce::String ("D minor"));
    }

    phazite::setTrackKeys (*song, tracks, std::nullopt);
    CHECK (std::none_of (tracks.begin(), tracks.end(),
                         [&song] (phazite::TrackID track) { return phazite::getTrackKey (*song, track).has_value(); }));
}

TEST_CASE ("Scales are saved by name")
{
    // Every scale offered goes to its name and back.
    CHECK (phazite::getKeyScales().size() == 9);

    for (const auto type : phazite::getKeyScales())
        CHECK (phazite::findScale (phazite::getSavedScaleName (type)) == type);

    CHECK (phazite::getSavedScaleName (phazite::Scale::harmonicMinor) == juce::String ("harmonicMinor"));

    // Each with its name as menus show it and its notes, a whole or a half step apart.
    CHECK (phazite::getScaleName (phazite::Scale::harmonicMinor) == juce::String ("Harmonic Minor"));
    CHECK (phazite::getScaleSteps (phazite::Scale::lydian) == std::vector { 0, 2, 4, 6, 7, 9, 11 });

    for (const auto type : phazite::getKeyScales())
    {
        const auto steps = phazite::getScaleSteps (type);
        REQUIRE (steps.size() == 7);

        for (size_t i = 1; i < steps.size(); ++i)
            CHECK ((steps[i] - steps[i - 1] == 1 || steps[i] - steps[i - 1] == 2 || steps[i] - steps[i - 1] == 3));
    }

    // Names are exact: no numbers, no other spelling.
    CHECK_FALSE (phazite::findScale ("3").has_value());
    CHECK_FALSE (phazite::findScale ("Dorian").has_value());
    CHECK_FALSE (phazite::findScale ("").has_value());
}

TEST_CASE ("The piano roll's key menu, Alt and the wheel, and an applied transform are undo steps of their own")
{
    auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto track = phazite::ensureTracks (song, 1).front();
    const auto clip = phazite::insertMidiClip (song, track, { phazite::BeatPosition(), beats (4.0) }, nullptr);
    REQUIRE (clip.isValid());
    const auto note = phazite::addNote (song, clip, 60, phazite::BeatPosition(), length (1.0), 100);
    REQUIRE (note.isValid());
    auto& undo = song.getUndoManager();
    undo.clearUndoHistory();
    phazite::test::UndoCheck check (song);

    check.check ("Set Key", [&] { phazite::chooseTrackKey (song, track, phazite::Key { 9, phazite::Scale::minor }); });
    CHECK (undo.getUndoDescription() == juce::String ("Set Key"));
    check.check ("Turn Key Off", [&] { phazite::chooseTrackKey (song, track, std::nullopt); });
    CHECK (undo.getUndoDescription() == juce::String ("Turn Key Off"));

    check.check ("Change Velocity", [&] { phazite::changeNoteVelocities (song, { note }, -10); });
    CHECK (noteOf (song, note).velocity == 90);
    CHECK (undo.getUndoDescription() == juce::String ("Change Velocity"));

    auto data = phazite::getNoteData (song, std::vector { note });
    data.front().pitch = 67;
    std::vector<phazite::NoteRef> replaced;
    check.check ("Transpose Notes", [&] { replaced = phazite::replaceNotes (song, { note }, data, "Transpose Notes"); });
    REQUIRE (replaced.size() == 1);
    CHECK (noteOf (song, replaced.front()).pitch == 67);
    CHECK (undo.getUndoDescription() == juce::String ("Transpose Notes"));
}

TEST_CASE ("Slide and portamento notes are switched by their commands, each one undo step, and their copies keep them")
{
    Notes n;
    const auto c = n.add (60, 4.0, 2.0);
    const auto e = n.add (64, 5.0, 1.0);
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);
    n.select ({ e });
    CHECK_FALSE (n.registry.find ("note.slide")->isTicked());

    // A slide note, saved on the song's note as well as shown.
    check.command (n.registry, "note.slide");
    CHECK (n.registry.find ("note.slide")->isTicked());
    CHECK (n.get (e).slide);
    CHECK_FALSE (n.get (e).portamento);
    CHECK_FALSE (n.get (c).slide);
    CHECK (phazite::describeUndo (&n.song) == juce::String ("Undo Slide Note"));

    // Portamento on both; with one of them already, it goes on for the other.
    n.select ({ c, e });
    check.command (n.registry, "note.portamento");
    CHECK (n.registry.find ("note.portamento")->isTicked());
    CHECK (n.get (c).portamento);
    CHECK_FALSE (n.registry.find ("note.slide")->isTicked()); // not every selected note is a slide note

    // Copies keep them.
    n.select ({ e });
    check.command (n.registry, "edit.duplicate");
    auto copies = 0;

    for (const auto& data : phazite::getClipNotes (n.song, n.clip))
        if (data.pitch == 64 && data.slide && data.portamento)
            ++copies;

    CHECK (copies == 2);

    // Undone, step by step.
    n.selection.deselectAll();
    check.stepBackOrForth ("Undo Duplicate", [&n] { phazite::undo (n.song); });
    check.stepBackOrForth ("Undo Portamento", [&n] { phazite::undo (n.song); });
    CHECK_FALSE (n.get (c).portamento);
    check.stepBackOrForth ("Undo Slide", [&n] { phazite::undo (n.song); });
    CHECK_FALSE (n.get (e).slide);
}

TEST_CASE ("Notes a clip doesn't play stay as they are: not selected, not pulled to its start, and no new ones are made there")
{
    // The clip from beat 4 to 8, with a clip after it, so that it can't grow; then trimmed to start at beat 5.
    Notes n;
    const auto next = phazite::insertMidiClip (n.song, n.track, { beats (8), beats (12) }, nullptr);
    REQUIRE (next.isValid());
    const auto before = n.add (60, 4.1, 0.5), cut = n.add (62, 4.75, 0.5), inside = n.add (64, 6.1, 0.5), last = n.add (65, 7.5, 0.5);
    phazite::trimClipStart (n.song, n.clip, beats (5));
    REQUIRE (n.rangeOf (n.clip) == phazite::BeatRange (beats (5), beats (8)));

    // The notes that play: the one the clip's start cuts, and those in it.
    const auto played = phazite::getClipNotesPlayed (n.song, n.clip);
    REQUIRE (played.size() == 3);
    CHECK (played[0].getRef() == cut);
    CHECK_FALSE (phazite::isNotePlayed (n.song, before));
    CHECK (phazite::isNotePlayed (n.song, cut));
    CHECK (phazite::getNoteNumbersPlayed (n.song, { n.clip }) == std::vector { 62, 64, 65 });

    // A selection holds those that play alone.
    n.select ({ before, cut, inside, last });
    CHECK (phazite::getSelectedNotes (n.selection) == std::vector { cut, inside, last });

    // A nudge back moves none while one starts before the clip already; forward, all of them, and the others stay.
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);
    step (check, n.song, "Nudge Notes", [&] { phazite::nudgeNotes (n.song, { cut, inside, last }, length (-0.25)); });
    CHECK (n.startOf (cut) == doctest::Approx (4.75));
    CHECK (n.startOf (inside) == doctest::Approx (6.1));
    step (check, n.song, "Nudge Note", [&] { phazite::nudgeNotes (n.song, { inside }, length (-0.25)); });
    CHECK (n.startOf (inside) == doctest::Approx (5.85));
    step (check, n.song, "Nudge Note", [&] { phazite::nudgeNotes (n.song, { inside }, length (0.25)); });

    // A move in pitch alone leaves the cut note where it starts, and a quantize puts it no earlier.
    step (check, n.song, "Move Note", [&] { phazite::moveNote (n.song, cut, beats (4.75), 63); });
    CHECK (n.startOf (cut) == doctest::Approx (4.75));
    CHECK (n.get (cut).pitch == 63);
    auto moved = n.get (cut);
    moved.start = beats (4.5);
    std::vector<phazite::NoteRef> replaced;
    step (check, n.song, "Quantize Note", [&] { replaced = phazite::replaceNotes (n.song, { cut }, { moved }); });
    REQUIRE (replaced.size() == 1);
    CHECK (replaced[0] == cut);
    CHECK (n.startOf (cut) == doctest::Approx (4.75));
    CHECK (n.startOf (before) == doctest::Approx (4.1));

    // Copies that wouldn't play aren't made: Duplicate puts them after the last note, past the clip's end, where the next
    // clip is; and a new note there is refused by the model's function for the editors.
    step (check, n.song, "Duplicate Notes", [&] { CHECK (phazite::duplicateNotes (n.song, { inside, last }).empty()); });
    CHECK (n.numNotesIn (n.clip) == 4);
    CHECK (n.rangeOf (n.clip) == phazite::BeatRange (beats (5), beats (8)));
    CHECK_FALSE (phazite::canPlayNoteAt (n.song, n.clip, beats (8.5)));
    CHECK (phazite::canPlayNoteAt (n.song, n.clip, beats (7.9)));

    // Where the clip can grow, they are made as before, and the clip holds them.
    step (check, n.song, "Delete Clip", [&] { phazite::deleteClips (n.song, { next }, false); });
    std::vector<phazite::NoteRef> copies;
    step (check, n.song, "Duplicate Notes", [&] { copies = phazite::duplicateNotes (n.song, { inside, last }); });
    CHECK (copies.size() == 2);
    CHECK (n.rangeOf (n.clip).getEnd() > beats (9));
    CHECK (phazite::canPlayNoteAt (n.song, n.clip, beats (20)));
}

TEST_CASE (
    "No edit of a note that plays leaves it where its clip doesn't play it: a move, a nudge and a quantize stop before the next clip")
{
    // The clip from beat 4 to 8, with a clip after it, so that it can't grow: a note moved to beat 8 or later would be in
    // the clip without playing, which no editor shows, so nothing could bring it back but Undo.
    Notes n;
    const auto next = phazite::insertMidiClip (n.song, n.track, { beats (8), beats (12) }, nullptr);
    REQUIRE (next.isValid());
    const auto note = n.add (60, 7.0, 0.5), other = n.add (62, 5.0, 0.5);
    const auto latest = 8.0 - phazite::getMinimumNoteLength().inBeats();

    // A nudge takes the notes as far as the last of them can go, together.
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);
    step (check, n.song, "Nudge Notes", [&] { phazite::nudgeNotes (n.song, { note, other }, length (2.0)); });
    CHECK (phazite::isNotePlayed (n.song, note));
    CHECK (n.startOf (note) == doctest::Approx (latest));
    CHECK (n.startOf (other) == doctest::Approx (5.0 + latest - 7.0));
    CHECK (n.rangeOf (n.clip) == phazite::BeatRange (beats (4), beats (8)));
    step (check, n.song, "Nudge Notes", [&] { phazite::nudgeNotes (n.song, { note, other }, length (7.0 - latest)); });
    CHECK (n.startOf (note) == doctest::Approx (7.0));

    // A move stops there too, and comes back.
    step (check, n.song, "Move Note", [&] { phazite::moveNote (n.song, note, beats (9.5), 60); });
    CHECK (phazite::isNotePlayed (n.song, note));
    CHECK (n.startOf (note) == doctest::Approx (latest));
    step (check, n.song, "Move Note", [&] { phazite::moveNote (n.song, note, beats (7.0), 60); });
    CHECK (n.startOf (note) == doctest::Approx (7.0));

    // What replaces it, as a quantize to the clip's end would, stays where it plays, the same note.
    auto moved = n.get (note);
    moved.start = beats (8.0);
    std::vector<phazite::NoteRef> replaced;
    step (check, n.song, "Quantize Note", [&] { replaced = phazite::replaceNotes (n.song, { note }, { moved }); });
    REQUIRE (replaced.size() == 1);
    CHECK (replaced[0] == note);
    CHECK (phazite::isNotePlayed (n.song, note));
    CHECK (n.startOf (note) == doctest::Approx (latest));

    // A note that goes on past the clip, chopped: its pieces past the clip stay in the clip as its end was, unplayed, so
    // that the clip plays them once it covers them, as it would have played the note.
    phazite::NoteRef longNote;
    step (check, n.song, "Add Note", [&] { longNote = n.add (67, 7.0, 2.0); });
    std::vector<phazite::NoteData> pieces;

    for (const auto start : { 7.0, 7.5, 8.0, 8.5 })
    {
        auto piece = n.get (longNote);
        piece.start = beats (start);
        piece.length = length (0.5);
        pieces.push_back (piece);
    }

    step (check, n.song, "Chop Note", [&] { CHECK (phazite::replaceNotes (n.song, { longNote }, pieces).size() == 4); });
    const auto held = phazite::getClipNotes (n.song, n.clip);
    CHECK (std::count_if (held.begin(), held.end(), [] (const auto& each) { return each.pitch == 67; }) == 4);
    const auto played = phazite::getClipNotesPlayed (n.song, n.clip);
    CHECK (std::count_if (played.begin(), played.end(), [] (const auto& each) { return each.pitch == 67; }) == 2);
    CHECK (std::any_of (held.begin(), held.end(),
                        [] (const auto& each) { return each.pitch == 67 && std::abs (each.start.inBeats() - 8.5) < 1.0e-6; }));

    // Where the clip can grow, a move takes the note as far as it is asked, and the clip grows to hold it, as before.
    step (check, n.song, "Delete Clip", [&] { phazite::deleteClips (n.song, { next }, false); });
    step (check, n.song, "Move Note", [&] { phazite::moveNote (n.song, note, beats (9.5), 60); });
    CHECK (n.startOf (note) == doctest::Approx (9.5));
    CHECK (phazite::isNotePlayed (n.song, note));
    CHECK (n.rangeOf (n.clip).getEnd() > beats (9.9));
    check.walkBackAndForth();
}

TEST_CASE ("A clip grows to hold a note and its aliases don't: they hold the note still, where they play it or not")
{
    // Notes.h, addNote(): the clip grows to the end of the bar, only into the empty space after it; and the note is in each
    // of the clip's aliases too, which grow for none of it. The clip from 4 to 8; its alias from 16 to 20 on another track,
    // with a clip right after it there, from 20 to 24.
    Notes n;
    const auto other = phazite::insertTrack (n.song, n.track, nullptr);
    REQUIRE (other.isValid());
    const auto aliases = phazite::copyClips (n.song, { n.clip }, true);
    REQUIRE (aliases.size() == 1);
    const auto alias = aliases.front();
    phazite::moveClip (n.song, alias, beats (16.0), other);
    REQUIRE (phazite::getClipTrack (n.song, alias) == other);
    const auto after = phazite::insertMidiClip (n.song, other, { beats (20.0), beats (24.0) }, nullptr);
    REQUIRE (after.isValid());
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);
    const auto inAlias = [alias] (const phazite::NoteRef& note) { return phazite::NoteRef { alias, note.note }; };
    const auto inClip = [&n] (const phazite::NoteRef& note) { return phazite::NoteRef { n.clip, note.note }; };
    const auto aliasStays = [&]
    {
        CHECK (n.rangeOf (alias) == phazite::BeatRange (beats (16.0), beats (20.0)));
        CHECK (n.rangeOf (after) == phazite::BeatRange (beats (20.0), beats (24.0)));
        CHECK (n.numNotesIn (alias) == n.numNotesIn (n.clip));
    };

    // Added in the clip at beat 11.5: the clip grows to 12, where the bar ends; the alias holds the note at its beat 7.5
    // (23.5), which it doesn't play.
    phazite::NoteRef late;
    step (check, n.song, "Add Note", [&] { late = n.add (60, 11.5, 0.5); });
    CHECK (n.rangeOf (n.clip) == phazite::BeatRange (beats (4.0), beats (12.0)));
    aliasStays();
    CHECK (n.startOf (inAlias (late)) == doctest::Approx (23.5));
    CHECK (phazite::isNotePlayed (n.song, late));
    CHECK_FALSE (phazite::isNotePlayed (n.song, inAlias (late)));

    // Duplicated past the clip's end: it grows to the next bar's end, 16; the alias holds the copy at 24, unplayed.
    std::vector<phazite::NoteRef> copies;
    step (check, n.song, "Duplicate Note", [&] { copies = phazite::duplicateNotes (n.song, { late }); });
    REQUIRE (copies.size() == 1);
    CHECK (n.startOf (copies[0]) == doctest::Approx (12.0));
    CHECK (n.rangeOf (n.clip) == phazite::BeatRange (beats (4.0), beats (16.0)));
    aliasStays();
    CHECK (n.startOf (inAlias (copies[0])) == doctest::Approx (24.0));
    CHECK_FALSE (phazite::isNotePlayed (n.song, inAlias (copies[0])));

    // Added through the alias past its end, where the clip after it keeps it from growing: it stays in the alias without
    // playing, as addNote() says, and the clip, which reaches its beat 5.5 (9.5) now, plays it.
    phazite::NoteRef through;
    step (check, n.song, "Add Note", [&] { through = phazite::addNote (n.song, alias, 62, beats (21.5), length (0.5), 100); });
    REQUIRE (through.isValid());
    aliasStays();
    CHECK (n.startOf (through) == doctest::Approx (21.5));
    CHECK_FALSE (phazite::isNotePlayed (n.song, through));
    CHECK (n.startOf (inClip (through)) == doctest::Approx (9.5));
    CHECK (phazite::isNotePlayed (n.song, inClip (through)));
    CHECK (n.rangeOf (n.clip) == phazite::BeatRange (beats (4.0), beats (16.0)));

    // A copy through the alias that would land past it isn't made (duplicateNotes()), in either clip, though the clip would
    // play it there.
    phazite::NoteRef last;
    step (check, n.song, "Add Note", [&] { last = phazite::addNote (n.song, alias, 64, beats (19.0), length (1.0), 100); });
    REQUIRE (last.isValid());
    const auto held = n.numNotesIn (n.clip);
    step (check, n.song, "Duplicate Note", [&] { CHECK (phazite::duplicateNotes (n.song, { last }).empty()); });
    CHECK (n.numNotesIn (n.clip) == held);
    aliasStays();
    check.walkBackAndForth();
}

namespace
{
// Beats as text, to a millionth.
juce::String formatBeats (double beats)
{
    return juce::String (std::round (beats * 1.0e6) / 1.0e6 + 0.0, 6);
}
} // namespace

TEST_CASE ("A clip's notes come in the order it holds them: added after the others, deleted from their places and put back there "
           "by Undo, and so saved and opened again")
{
    // Notes.h, getClipNotes(): "in the order the clip holds them", which the piano roll and the commands' goldens read.
    phazite::test::TestSong song;
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 8.0);
    const auto getSong = [&song]() -> phazite::Song& { return *phazite::findSong (song.context); };

    // Each note as its pitch, velocity and start, in order; and their IDs.
    const auto order = [&]
    {
        juce::StringArray notes;

        for (const auto& note : phazite::getClipNotes (getSong(), clip))
            notes.add (juce::String (note.pitch) + "/" + juce::String (note.velocity) + "@" + formatBeats (note.start.inBeats()));

        return notes.joinIntoString (" ").toStdString();
    };

    const auto ids = [&]
    {
        std::vector<phazite::NoteID> made;

        for (const auto& note : phazite::getClipNotes (getSong(), clip))
            made.push_back (note.id);

        return made;
    };

    std::vector<phazite::NoteID> before;
    std::string inOrder;

    {
        auto& s = getSong();
        phazite::test::UndoCheck check (song.context);
        const auto add = [&] (int pitch, double start, int velocity)
        {
            phazite::NoteRef note;
            step (check, s, "Add Note", [&] { note = phazite::addNote (s, clip, pitch, beats (start), length (0.5), velocity); });
            REQUIRE (note.isValid());
            return note;
        };

        // Two notes of one pitch at one start (b and d) among them.
        const auto a = add (64, 2.0, 100), b = add (60, 0.0, 100), c = add (67, 1.0, 100), d = add (60, 0.0, 50), e = add (62, 3.0, 100);
        CHECK (order() == "64/100@2.000000 60/100@0.000000 67/100@1.000000 60/50@0.000000 62/100@3.000000");

        // Deleted, the others keep their order; undone, the note is back at its place.
        step (check, s, "Delete Note", [&] { phazite::deleteNotes (s, { c }); });
        CHECK (order() == "64/100@2.000000 60/100@0.000000 60/50@0.000000 62/100@3.000000");
        check.stepBackOrForth ("Undo", [&s] { phazite::undo (s); });
        CHECK (order() == "64/100@2.000000 60/100@0.000000 67/100@1.000000 60/50@0.000000 62/100@3.000000");

        // Moved, a note keeps its place.
        step (check, s, "Move Note", [&] { phazite::moveNote (s, a, beats (2.5), 65); });
        CHECK (order() == "65/100@2.500000 60/100@0.000000 67/100@1.000000 60/50@0.000000 62/100@3.000000");

        // Copies go after the others, in the order of their originals.
        step (check, s, "Duplicate Notes", [&] { CHECK (phazite::duplicateNotes (s, { b, d }).size() == 2); });
        CHECK (order() == "65/100@2.500000 60/100@0.000000 67/100@1.000000 60/50@0.000000 62/100@3.000000 60/100@0.500000 60/50@0.500000");

        // A note replaced, as a transform replaces it, keeps its ID and goes after the others.
        auto quantized = phazite::getNoteData (s, std::vector { e });
        REQUIRE (quantized.size() == 1);
        quantized[0].start = beats (3.25);
        step (check, s, "Quantize Note", [&] { CHECK (phazite::replaceNotes (s, { e }, quantized) == std::vector { e }); });
        CHECK (order() == "65/100@2.500000 60/100@0.000000 67/100@1.000000 60/50@0.000000 60/100@0.500000 60/50@0.500000 62/100@3.250000");

        check.walkBackAndForth();
        inOrder = order();
        before = ids();
    }

    // Saved and opened again: the same notes, IDs and all, in the same order.
    song.reopen();
    CHECK (order() == inOrder);
    CHECK (ids() == before);
}

TEST_CASE ("Every value of a note is kept within its limits, at both ends, by every edit")
{
    Notes n; // the clip from beat 4 to 8
    n.song.getUndoManager().clearUndoHistory();
    std::optional<phazite::test::UndoCheck> check;
    check.emplace (n.song);
    const auto edit = [&] (const juce::String& name, const std::function<void()>& change) { step (*check, n.song, name, change); };

    SUBCASE ("The pitch, from 0 to 127; a chord moved as far as its top or bottom note can go, keeping its shape")
    {
        phazite::NoteRef high, low;
        edit ("Add Note", [&] { high = n.add (130, 4.0, 1.0); });
        edit ("Add Note", [&] { low = n.add (-3, 5.0, 1.0); });
        CHECK (n.get (high).pitch == 127);
        CHECK (n.get (low).pitch == 0);

        edit ("Move Note", [&] { phazite::moveNote (n.song, low, beats (5.0), 60); });
        edit ("Move Note", [&] { phazite::moveNote (n.song, low, beats (5.0), -5); });
        CHECK (n.get (low).pitch == 0);
        edit ("Move Note", [&] { phazite::moveNote (n.song, high, beats (4.0), 60); });
        edit ("Move Note", [&] { phazite::moveNote (n.song, high, beats (4.0), 200); });
        CHECK (n.get (high).pitch == 127);
        edit ("Place Note", [&] { phazite::placeNote (n.song, high, beats (4.0), 300, length (1.0)); });
        CHECK (n.get (high).pitch == 127);

        // Placed as a drag places them, and replaced as a transform replaces them: kept within 0 and 127 as well.
        edit ("Place Notes",
              [&]
              {
                  auto up = n.get (high), down = n.get (low);
                  up.start = beats (4.5);
                  up.pitch = 300;
                  down.start = beats (5.5);
                  down.pitch = -10;
                  phazite::placeNotes (n.song, { up, down });
              });
        CHECK (n.get (high).pitch == 127);
        CHECK (n.get (low).pitch == 0);
        edit ("Replace Notes",
              [&]
              {
                  auto up = n.get (high), down = n.get (low);
                  up.pitch = 400;
                  up.start = beats (4.0);
                  down.pitch = -40;
                  down.start = beats (5.0);
                  CHECK (phazite::replaceNotes (n.song, { high, low }, { up, down }) == std::vector { high, low });
              });
        CHECK (n.get (high).pitch == 127);
        CHECK (n.get (low).pitch == 0);

        // A chord whose top is 120, up an octave: it goes up 7, as far as its top can.
        std::vector<phazite::NoteRef> chord;
        edit ("Add Chord", [&] { chord = { n.add (100, 6.0, 0.5), n.add (113, 6.0, 0.5), n.add (120, 6.0, 0.5) }; });
        edit ("Transpose Notes", [&] { phazite::transposeNotes (n.song, chord, 12); });
        CHECK (n.get (chord[0]).pitch == 107);
        CHECK (n.get (chord[1]).pitch == 120);
        CHECK (n.get (chord[2]).pitch == 127);
        edit ("Transpose Notes", [&] { phazite::transposeNotes (n.song, chord, -120); });
        CHECK (n.get (chord[0]).pitch == 0);
        CHECK (n.get (chord[1]).pitch == 13);
        CHECK (n.get (chord[2]).pitch == 20);

        // Octave Up at the top: as far as the top note goes, then nothing.
        std::vector<phazite::NoteRef> top;
        edit ("Add Chord", [&] { top = { n.add (110, 7.0, 0.5), n.add (118, 7.0, 0.5) }; });
        n.select (top);
        check->command (n.registry, "note.octaveUp");
        CHECK (n.get (top[0]).pitch == 119);
        CHECK (n.get (top[1]).pitch == 127);
        check->command (n.registry, "note.octaveUp");
        CHECK (n.get (top[0]).pitch == 119);
        CHECK (n.get (top[1]).pitch == 127);
        n.selection.deselectAll();

        // The notes the chord lane puts in a clip.
        edit ("Set Clip Notes",
              [&] { phazite::setClipNotes (n.song, n.clip, { noteData (300, 4.0, 1.0, 100), noteData (-5, 5.0, 1.0, 100) }); });
        const auto set = phazite::getClipNotes (n.song, n.clip);
        REQUIRE (set.size() == 2);
        CHECK (set[0].pitch == 127);
        CHECK (set[1].pitch == 0);
    }

    SUBCASE ("The velocity, from 1 to 127")
    {
        phazite::NoteRef quiet, loud;
        edit ("Add Note", [&] { quiet = n.add (60, 4.0, 1.0, 0); });
        edit ("Add Note", [&] { loud = n.add (62, 5.0, 1.0, 300); });
        CHECK (n.get (quiet).velocity == 1);
        CHECK (n.get (loud).velocity == 127);

        edit ("Set Velocity", [&] { phazite::setNoteVelocities (n.song, { quiet, loud }, 64); });
        edit ("Set Velocity", [&] { phazite::setNoteVelocities (n.song, { quiet, loud }, 0); });
        CHECK (n.get (quiet).velocity == 1);
        CHECK (n.get (loud).velocity == 1);
        edit ("Set Velocity", [&] { phazite::setNoteVelocities (n.song, { quiet, loud }, 200); });
        CHECK (n.get (quiet).velocity == 127);

        check->check ("Change Velocity", [&] { phazite::changeNoteVelocities (n.song, { quiet, loud }, -200); });
        CHECK (n.get (quiet).velocity == 1);
        CHECK (n.get (loud).velocity == 1);
        check->check ("Change Velocity", [&] { phazite::changeNoteVelocities (n.song, { quiet, loud }, 200); });
        CHECK (n.get (quiet).velocity == 127);
        CHECK (n.get (loud).velocity == 127);

        // Replaced as a transform replaces them, and put in a clip as the chord lane puts them: within 1 and 127.
        edit ("Replace Notes",
              [&]
              {
                  auto softer = n.get (quiet), louder = n.get (loud);
                  softer.velocity = 0;
                  louder.velocity = 300;
                  CHECK (phazite::replaceNotes (n.song, { quiet, loud }, { softer, louder }) == std::vector { quiet, loud });
              });
        CHECK (n.get (quiet).velocity == 1);
        CHECK (n.get (loud).velocity == 127);

        // placeNotes() places a note's start, pitch and length only: the velocity it is given isn't the note's.
        edit ("Place Notes",
              [&]
              {
                  auto placed = n.get (quiet);
                  placed.start = beats (4.5);
                  placed.velocity = 300;
                  phazite::placeNotes (n.song, { placed });
              });
        CHECK (n.get (quiet).velocity == 1);

        edit ("Set Clip Notes",
              [&] { phazite::setClipNotes (n.song, n.clip, { noteData (60, 4.0, 1.0, -5), noteData (62, 5.0, 1.0, 500) }); });
        const auto set = phazite::getClipNotes (n.song, n.clip);
        REQUIRE (set.size() == 2);
        CHECK (set[0].velocity == 1);
        CHECK (set[1].velocity == 127);
    }

    SUBCASE ("The channel, from 1 to 16")
    {
        phazite::NoteRef first, last;
        edit ("Add Note", [&] { first = phazite::addNote (n.song, n.clip, 60, beats (4.0), length (1.0), 100, 0); });
        edit ("Add Note", [&] { last = phazite::addNote (n.song, n.clip, 62, beats (5.0), length (1.0), 100, 99); });
        CHECK (n.get (first).channel == 1);
        CHECK (n.get (last).channel == 16);

        edit ("Set Channel", [&] { phazite::setNoteChannels (n.song, { first, last }, 17); });
        CHECK (n.get (first).channel == 16);
        CHECK (n.get (last).channel == 16);
        edit ("Set Channel", [&] { phazite::setNoteChannels (n.song, { first, last }, 0); });
        CHECK (n.get (first).channel == 1);
        CHECK (n.get (last).channel == 1);

        // Replaced as a transform replaces them, and put in a clip as the chord lane puts them: within 1 and 16.
        edit ("Replace Notes",
              [&]
              {
                  auto low = n.get (first), high = n.get (last);
                  low.channel = -3;
                  high.channel = 40;
                  CHECK (phazite::replaceNotes (n.song, { first, last }, { low, high }) == std::vector { first, last });
              });
        CHECK (n.get (first).channel == 1);
        CHECK (n.get (last).channel == 16);

        // placeNotes() places a note's start, pitch and length only: the channel it is given isn't the note's.
        edit ("Place Notes",
              [&]
              {
                  auto placed = n.get (first);
                  placed.start = beats (4.5);
                  placed.channel = 9;
                  phazite::placeNotes (n.song, { placed });
              });
        CHECK (n.get (first).channel == 1);

        edit ("Set Clip Notes",
              [&]
              {
                  auto low = noteData (60, 4.0, 1.0, 100), high = noteData (62, 5.0, 1.0, 100);
                  low.channel = 0;
                  high.channel = 17;
                  phazite::setClipNotes (n.song, n.clip, { low, high });
              });
        const auto set = phazite::getClipNotes (n.song, n.clip);
        REQUIRE (set.size() == 2);
        CHECK (set[0].channel == 1);
        CHECK (set[1].channel == 16);
    }

    SUBCASE ("The length, at least the shortest a note can be")
    {
        const auto shortest = phazite::getMinimumNoteLength().inBeats();
        phazite::NoteRef note;
        edit ("Add Note", [&] { note = n.add (60, 4.0, 0.0); });
        CHECK (n.get (note).length.inBeats() == doctest::Approx (shortest));

        edit ("Set Note Length", [&] { phazite::setNoteLength (n.song, note, length (1.0)); });
        edit ("Lengthen Notes", [&] { phazite::changeNoteLengths (n.song, { note }, length (-10.0)); });
        CHECK (n.get (note).length.inBeats() == doctest::Approx (shortest));
        edit ("Set Note Length", [&] { phazite::setNoteLength (n.song, note, length (-1.0)); });
        CHECK (n.get (note).length.inBeats() == doctest::Approx (shortest));
        edit ("Place Note", [&] { phazite::placeNote (n.song, note, beats (5.0), 60, length (1.0)); });
        edit ("Place Note", [&] { phazite::placeNote (n.song, note, beats (5.0), 60, length (0.0)); });
        CHECK (n.get (note).length.inBeats() == doctest::Approx (shortest));

        // setClipNotes() applies no shortest length today: a note given none, or less than the shortest, is put in as given.
        // Pinned as it is (its comment in Notes.h names no limit), so that a change of it is seen.
        edit ("Set Clip Notes",
              [&] { phazite::setClipNotes (n.song, n.clip, { noteData (60, 4.0, 0.0, 100), noteData (62, 5.0, shortest / 2.0, 100) }); });
        const auto set = phazite::getClipNotes (n.song, n.clip);
        REQUIRE (set.size() == 2);
        CHECK (set[0].length.inBeats() == doctest::Approx (0.0));
        CHECK (set[1].length.inBeats() == doctest::Approx (shortest / 2.0));
    }

    SUBCASE ("A controller's value, from 0 to its lane's highest; its number, from 0 to 127; its channel, from 1 to 16")
    {
        using phazite::ControlKind;
        const phazite::ControlLane modWheel { ControlKind::controller, 1, 1 };
        const phazite::ControlLane bend { ControlKind::pitchWheel, 0, 1 };

        // A lane's values as they read, in time order.
        const auto valuesOf = [&n] (const phazite::ControlLane& lane)
        {
            std::vector<int> values;

            for (const auto& point : phazite::getControlValues (n.song, { n.clip }, lane))
                values.push_back (point.value);

            return values;
        };

        const auto hasLane = [&n] (const phazite::ControlLane& lane)
        {
            const auto lanes = phazite::getControlLanes (n.song, { n.clip });
            return std::find (lanes.begin(), lanes.end(), lane) != lanes.end();
        };

        // The value: added one at a time, and many at once.
        edit ("Add Control", [&] { phazite::addControl (n.song, n.clip, beats (4.5), ControlKind::controller, 1, 300, 1); });
        edit ("Add Control", [&] { phazite::addControl (n.song, n.clip, beats (5.0), ControlKind::controller, 1, -20, 1); });
        CHECK (valuesOf (modWheel) == std::vector { 127, 0 });
        edit ("Add Controls",
              [&]
              {
                  phazite::addControls (
                      n.song, n.clip,
                      { { beats (4.5), ControlKind::pitchWheel, 0, 20000, 1 }, { beats (5.0), ControlKind::pitchWheel, 0, -5, 1 } });
              });
        CHECK (bend.getHighestValue() == 16383);
        CHECK (valuesOf (bend) == std::vector { 16383, 0 });

        // Drawn: within the lane's range too.
        edit ("Draw Mod Wheel",
              [&]
              {
                  phazite::setControlValues (n.song, n.clip, modWheel, { beats (6.0), beats (7.0) },
                                             { { {}, beats (6.0), 500, 0 }, { {}, beats (6.5), -5, 0 } });
              });
        CHECK (valuesOf (modWheel) == std::vector { 127, 0, 127, 0 });

        // The controller's number: from 0 to 127.
        edit ("Add Control", [&] { phazite::addControl (n.song, n.clip, beats (4.5), ControlKind::controller, 200, 64, 1); });
        edit ("Add Control", [&] { phazite::addControl (n.song, n.clip, beats (4.5), ControlKind::controller, -3, 65, 1); });
        CHECK (hasLane ({ ControlKind::controller, 127, 1 }));
        CHECK (hasLane ({ ControlKind::controller, 0, 1 }));
        CHECK (valuesOf ({ ControlKind::controller, 127, 1 }) == std::vector { 64 });
        CHECK (valuesOf ({ ControlKind::controller, 0, 1 }) == std::vector { 65 });

        // The channel: from 1 to 16.
        edit ("Add Control", [&] { phazite::addControl (n.song, n.clip, beats (4.5), ControlKind::controller, 7, 66, 0); });
        edit ("Add Control", [&] { phazite::addControl (n.song, n.clip, beats (4.5), ControlKind::controller, 7, 67, 99); });
        CHECK (valuesOf ({ ControlKind::controller, 7, 1 }) == std::vector { 66 });
        CHECK (valuesOf ({ ControlKind::controller, 7, 16 }) == std::vector { 67 });
        CHECK (phazite::makeControlLane (ControlKind::controller, 300, 40) == phazite::ControlLane { ControlKind::controller, 127, 16 });
        CHECK (phazite::makeControlLane (ControlKind::controller, -1, -1) == phazite::ControlLane { ControlKind::controller, 0, 1 });
    }

    SUBCASE ("The start, kept where the clip plays the note, both ways; and the clip grown no further than the next on its track")
    {
        // The next clip from beat 12: the clip can grow to 12, and a note can start no later than the shortest note before it.
        const auto next = phazite::insertMidiClip (n.song, n.track, { beats (12.0), beats (16.0) }, nullptr);
        REQUIRE (next.isValid());
        check.reset();
        n.song.getUndoManager().clearUndoHistory();
        check.emplace (n.song);
        const auto latest = 12.0 - phazite::getMinimumNoteLength().inBeats();
        const auto nextStays = [&]
        {
            CHECK (n.rangeOf (next) == phazite::BeatRange (beats (12.0), beats (16.0)));
            CHECK (n.rangeOf (n.clip).getEnd() <= beats (12.0));
        };

        phazite::NoteRef note;
        edit ("Add Note", [&] { note = n.add (60, 5.0, 0.5); });

        // Before the clip's start: to its start.
        edit ("Move Note", [&] { phazite::moveNote (n.song, note, beats (2.0), 60); });
        CHECK (n.startOf (note) == doctest::Approx (4.0));
        edit ("Nudge Note", [&] { phazite::nudgeNotes (n.song, { note }, length (0.5)); });
        edit ("Nudge Note", [&] { phazite::nudgeNotes (n.song, { note }, length (-10.0)); });
        CHECK (n.startOf (note) == doctest::Approx (4.0));
        edit ("Place Note", [&] { phazite::placeNote (n.song, note, beats (1.0), 60, length (0.5)); });
        CHECK (n.startOf (note) == doctest::Approx (4.0));
        edit ("Place Notes",
              [&]
              {
                  auto placed = n.get (note);
                  placed.start = beats (0.0);
                  phazite::placeNotes (n.song, { placed });
              });
        CHECK (n.startOf (note) == doctest::Approx (4.0));

        // Past where the clip can grow: as late as it still plays, the clip grown to the next one and no further.
        edit ("Move Note", [&] { phazite::moveNote (n.song, note, beats (30.0), 60); });
        CHECK (n.startOf (note) == doctest::Approx (latest));
        CHECK (phazite::isNotePlayed (n.song, note));
        nextStays();
        CHECK (n.rangeOf (n.clip).getEnd() == beats (12.0));
        edit ("Move Note", [&] { phazite::moveNote (n.song, note, beats (5.0), 60); });
        edit ("Nudge Note", [&] { phazite::nudgeNotes (n.song, { note }, length (30.0)); });
        CHECK (n.startOf (note) == doctest::Approx (latest));
        edit ("Move Note", [&] { phazite::moveNote (n.song, note, beats (5.0), 60); });
        edit ("Place Note", [&] { phazite::placeNote (n.song, note, beats (30.0), 60, length (0.5)); });
        CHECK (n.startOf (note) == doctest::Approx (latest));
        CHECK (phazite::isNotePlayed (n.song, note));
        nextStays();

        // Made longer than the room: the clip grows to the next one, no further, whatever lengthens it.
        edit ("Move Note", [&] { phazite::moveNote (n.song, note, beats (6.0), 60); });
        edit ("Lengthen Notes", [&] { phazite::changeNoteLengths (n.song, { note }, length (30.0)); });
        nextStays();
        edit ("Set Note Length", [&] { phazite::setNoteLength (n.song, note, length (40.0)); });
        nextStays();
        edit ("Place Note", [&] { phazite::placeNote (n.song, note, beats (6.0), 60, length (50.0)); });
        nextStays();
        edit ("Place Notes",
              [&]
              {
                  auto placed = n.get (note);
                  placed.length = length (60.0);
                  phazite::placeNotes (n.song, { placed });
              });
        nextStays();
        CHECK (n.get (note).length.inBeats() == doctest::Approx (60.0)); // the note keeps its length, past the clip
        edit ("Add Note", [&] { n.add (62, 9.0, 20.0); });
        nextStays();
        edit ("Duplicate Note", [&] { CHECK (phazite::duplicateNotes (n.song, { note }).empty()); });
        nextStays();
        CHECK (n.numNotesIn (next) == 0);
    }

    check->walkBackAndForth();
}

TEST_CASE ("A nudge through an alias whose notes start further into them stops at that alias's start, in every clip of the group")
{
    // Notes.h, nudgeNotes(): none "to before its clip's start or to where its clip doesn't play it". The clip from beat 4 to 8
    // with a note at its beat 1.5; its alias from 21 to 24, trimmed to start a beat into its notes (they count from 20).
    Notes n;
    const auto note = n.add (60, 5.5, 0.5);
    const auto aliases = phazite::copyClips (n.song, { n.clip }, true);
    REQUIRE (aliases.size() == 1);
    const auto alias = aliases.front();
    phazite::moveClip (n.song, alias, beats (20.0), n.track);
    phazite::trimClipStart (n.song, alias, beats (21.0));
    REQUIRE (n.rangeOf (alias) == phazite::BeatRange (beats (21.0), beats (24.0)));
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);
    const phazite::NoteRef inAlias { alias, note.note };

    // Back by a beat through the alias: half a beat only, to its start; the clip's copy, the same note, with it.
    step (check, n.song, "Nudge Note", [&] { phazite::nudgeNotes (n.song, { inAlias }, length (-1.0)); });
    CHECK (n.startOf (inAlias) == doctest::Approx (21.0));
    CHECK (n.startOf (note) == doctest::Approx (5.0));
    CHECK (phazite::isNotePlayed (n.song, inAlias));

    // Through the clip, which starts with its notes: as far as asked, to where the alias no longer plays it.
    step (check, n.song, "Nudge Note", [&] { phazite::nudgeNotes (n.song, { note }, length (-1.0)); });
    CHECK (n.startOf (note) == doctest::Approx (4.0));
    CHECK (n.startOf (inAlias) == doctest::Approx (20.0));
    CHECK_FALSE (phazite::isNotePlayed (n.song, inAlias));
    check.walkBackAndForth();
}

//==============================================================================
// What changes a clip's notes from outside the notes' edits (Notes.h), each by the song and IDs.
namespace
{
// A clip's notes as the song holds them, each as its ID, pitch, place among the clip's notes, length and velocity, and
// whether it is muted, in the clip's order: what two clips holding the same notes have alike.
std::vector<std::string> describeNotes (const phazite::Song& song, phazite::ClipID clip, bool withIDs = true)
{
    std::vector<std::string> notes;
    const auto contentStart = phazite::getContentStart (phazite::findClipState (song, clip));

    for (const auto& note : phazite::getClipNotes (song, clip))
        notes.push_back ((withIDs ? juce::String ((juce::int64) note.id.getRaw()) + " " : juce::String()).toStdString()
                         + std::to_string (note.pitch) + " " + phazite::writeValue ((note.start - contentStart).inBeats()).toStdString()
                         + " " + phazite::writeValue (note.length.inBeats()).toStdString() + " " + std::to_string (note.velocity)
                         + (note.muted ? " muted" : ""));

    return notes;
}

// A clip's controllers' values, each as its number, its place among the clip's notes and its value, in the clip's order.
std::vector<std::string> describeControls (const phazite::Song& song, phazite::ClipID clip)
{
    std::vector<std::string> controls;
    const auto contentStart = phazite::getContentStart (phazite::findClipState (song, clip));

    for (const auto& control : phazite::getClipControls (song, clip))
        controls.push_back (std::to_string (control.number) + " "
                            + phazite::writeValue ((control.beat - contentStart).inBeats()).toStdString() + " "
                            + std::to_string (control.value));

    return controls;
}

using Lines = std::vector<std::string>;
} // namespace

TEST_CASE ("Whether a note is there, by its clip and its ID")
{
    Notes n;
    const auto note = n.add (60, 5.0, 1.0);
    const auto other = phazite::insertMidiClip (n.song, n.track, { beats (12), beats (16) }, nullptr);
    REQUIRE (other.isValid());

    CHECK (phazite::hasNote (n.song, note));
    CHECK_FALSE (phazite::hasNote (n.song, { other, note.note })); // the same ID in another clip
    CHECK_FALSE (phazite::hasNote (n.song, { n.clip, {} }));
    CHECK_FALSE (phazite::hasNote (n.song, { {}, note.note }));

    {
        const phazite::UndoStep step (n.song, "Delete Note");
        phazite::deleteNotes (n.song, { note });
    }

    CHECK_FALSE (phazite::hasNote (n.song, note));
    phazite::undo (n.song);
    CHECK (phazite::hasNote (n.song, note));
}

TEST_CASE ("Scaling clips' notes and controllers' values scales each alias group's once, however many of its clips are named")
{
    Notes n;
    n.add (60, 5.0, 1.0); // a beat into the clip, which starts at beat 4
    phazite::addControl (n.song, n.clip, beats (6.0), phazite::ControlKind::controller, 64, 127);
    const auto aliases = phazite::copyClips (n.song, { n.clip }, true);
    REQUIRE (aliases.size() == 1);
    const auto alias = aliases.front();
    const auto loose = phazite::insertMidiClip (n.song, n.track, { beats (20), beats (24) }, nullptr);
    phazite::addNote (n.song, loose, 62, beats (22.0), length (0.5), 90);
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);

    // The clip named twice and its alias once: each scaled once, its notes' starts and lengths and its values' beats.
    step (check, n.song, "Change Tempo", [&] { phazite::scaleClipContent (n.song, { n.clip, alias, n.clip, loose }, 2.0); });

    for (const auto clip : { n.clip, alias })
    {
        CHECK (describeNotes (n.song, clip, false) == Lines { "60 2 2 100" });
        CHECK (describeControls (n.song, clip) == Lines { "64 4 127" });
    }

    CHECK (describeNotes (n.song, loose, false) == Lines { "62 4 1 90" });

    // One clip of a group named: the group's notes, its alias's too, and no other clip's.
    step (check, n.song, "Change Tempo", [&] { phazite::scaleClipContent (n.song, { alias }, 0.5); });
    CHECK (describeNotes (n.song, n.clip, false) == Lines { "60 1 1 100" });
    CHECK (describeNotes (n.song, alias) == describeNotes (n.song, n.clip));
    CHECK (describeControls (n.song, n.clip) == Lines { "64 2 127" });
    CHECK (describeNotes (n.song, loose, false) == Lines { "62 4 1 90" });
    check.walkBackAndForth();
}

TEST_CASE ("A clip's notes made like another song's clip's: those it hasn't come, those both have take its values, the others go")
{
    Notes n;
    const auto kept = n.add (60, 5.0, 1.0);
    const auto gone = n.add (62, 6.0, 1.0);
    phazite::addControl (n.song, n.clip, beats (6.5), phazite::ControlKind::controller, 64, 127);
    const auto then = phazite::copyWhole (n.song, phazite::findClipState (n.song, n.clip)); // as another song holds it
    const auto notesThen = describeNotes (n.song, n.clip);
    const auto controlsThen = describeControls (n.song, n.clip);

    const auto changeSince = [&]
    {
        const phazite::UndoStep step (n.song, "Edit Notes");
        phazite::transposeNotes (n.song, { kept }, 5);
        phazite::deleteNotes (n.song, { gone });
        n.add (64, 4.5, 0.5);
        phazite::addControl (n.song, n.clip, beats (4.0), phazite::ControlKind::controller, 64, 0);
    };

    changeSince();
    REQUIRE (describeNotes (n.song, n.clip) != notesThen);
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);

    // The clip's notes and controllers' values as elements, whole: every property of each, in the element's order.
    const auto holdsThenEvents = [&]
    {
        const auto eventsOf = [] (const juce::ValueTree& clipState)
        {
            juce::ValueTree events ("EVENTS");

            for (const auto& child : clipState)
                if (child.hasType (phazite::schema::NOTE) || child.hasType (phazite::schema::CONTROL))
                    events.appendChild (child.createCopy(), nullptr);

            return events;
        };

        return eventsOf (phazite::copyWhole (n.song, phazite::findClipState (n.song, n.clip))).isEquivalentTo (eventsOf (then));
    };

    // In the song's undo history: as they were, in the other's order.
    step (check, n.song, "Restore", [&] { phazite::makeClipNotesLike (n.song, n.clip, then); });
    CHECK (describeNotes (n.song, n.clip) == notesThen);
    CHECK (describeControls (n.song, n.clip) == controlsThen);
    CHECK (holdsThenEvents());
    check.walkBackAndForth();

    // Outside it, while the song is written outside it: the same, and nothing to undo.
    changeSince();
    n.song.getUndoManager().clearUndoHistory();

    {
        const phazite::Song::OutsideHistory outside (n.song);
        CHECK (n.song.isWritingOutsideHistory());
        phazite::makeClipNotesLike (n.song, n.clip, then);
    }

    CHECK_FALSE (n.song.isWritingOutsideHistory());
    CHECK (describeNotes (n.song, n.clip) == notesThen);
    CHECK (describeControls (n.song, n.clip) == controlsThen);
    CHECK (holdsThenEvents());
    CHECK_FALSE (n.song.getUndoManager().canUndo());
}

TEST_CASE ("One value of a note set through its clip reaches the note in each of the clip's aliases, in one undo step")
{
    Notes n;
    const auto note = n.add (60, 5.0, 1.0);
    const auto aliases = phazite::copyClips (n.song, { n.clip }, true);
    REQUIRE (aliases.size() == 1);
    const phazite::NoteRef inAlias { aliases.front(), note.note };
    n.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (n.song);

    step (check, n.song, "Take the Other Branch's: a note's velocity",
          [&] { CHECK (phazite::setNoteValue (n.song, note, phazite::schema::noteVelocity, 40)); });
    CHECK (n.get (note).velocity == 40);
    CHECK (n.get (inAlias).velocity == 40);

    step (check, n.song, "Take the Other Branch's: a note's mute",
          [&] { CHECK (phazite::setNoteValue (n.song, inAlias, phazite::schema::noteMute, true)); });
    CHECK (n.get (note).muted);
    CHECK (n.get (inAlias).muted);

    // A void value takes it away.
    step (check, n.song, "Take What Was Before: a note's mute",
          [&] { CHECK (phazite::setNoteValue (n.song, note, phazite::schema::noteMute, juce::var())); });
    CHECK_FALSE (n.get (note).muted);
    CHECK_FALSE (n.get (inAlias).muted);
    check.walkBackAndForth();

    // A note that isn't there: nothing.
    const auto other = phazite::insertMidiClip (n.song, n.track, { beats (12), beats (16) }, nullptr);
    CHECK_FALSE (phazite::setNoteValue (n.song, { other, note.note }, phazite::schema::noteVelocity, 10));
}

TEST_CASE ("Notes taken out of one clip alone stay in its aliases, and outside the undo history")
{
    Notes n;
    const auto note = n.add (60, 5.0, 1.0);
    const auto stays = n.add (62, 6.0, 1.0);
    const auto aliases = phazite::copyClips (n.song, { n.clip }, true);
    REQUIRE (aliases.size() == 1);
    n.song.getUndoManager().clearUndoHistory();

    phazite::removeClipNotesAlone (n.song, n.clip, { note.note });
    CHECK_FALSE (phazite::hasNote (n.song, note));
    CHECK (phazite::hasNote (n.song, stays));
    CHECK (phazite::hasNote (n.song, { aliases.front(), note.note }));
    CHECK_FALSE (n.song.getUndoManager().canUndo());
}
