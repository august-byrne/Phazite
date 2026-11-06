// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "ui/input/KeyDispatcher.h"

#include <algorithm>

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
} // namespace

TEST_CASE ("Notes are placed in song beats, and their clip grows to hold them")
{
    Notes n;
    const auto note = n.add (60, 5.0, 1.0);
    CHECK (n.startOf (note) == doctest::Approx (5.0));
    const auto inClip = (double) phazite::findNoteState (n.song, note)[phazite::schema::noteStart];
    CHECK (inClip == doctest::Approx (1.0)); // in the clip, from its content's start

    // A note can't start before its clip, and one past its end makes it grow to the end of that bar.
    const auto early = n.add (62, 2.0, 1.0);
    CHECK (n.startOf (early) == doctest::Approx (4.0));

    n.add (64, 9.0, 0.5);
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (12.0));

    phazite::moveNote (n.song, note, beats (13.5), 72);
    CHECK (n.startOf (note) == doctest::Approx (13.5));
    CHECK (n.get (note).pitch == 72);
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (16.0));

    phazite::setNoteLength (n.song, note, length (0.0));
    CHECK (n.get (note).length.inBeats() == doctest::Approx (phazite::getMinimumNoteLength().inBeats()));
}

TEST_CASE ("Transposing and nudging keep the notes together, within their limits")
{
    Notes n;
    const auto low = n.add (2, 4.5, 1.0);
    const auto high = n.add (60, 5.0, 1.0);
    const std::vector<phazite::NoteRef> both { low, high };

    // The lowest note can only go down 2, so the chord goes down 2.
    phazite::transposeNotes (n.song, both, -12);
    CHECK (n.get (low).pitch == 0);
    CHECK (n.get (high).pitch == 58);

    // The earliest can only go back half a beat, to the clip's start.
    phazite::nudgeNotes (n.song, both, length (-2.0));
    CHECK (n.startOf (low) == doctest::Approx (4.0));
    CHECK (n.startOf (high) == doctest::Approx (4.5));

    phazite::changeNoteLengths (n.song, both, length (0.5));
    CHECK (n.get (high).length.inBeats() == doctest::Approx (1.5));

    phazite::toggleNotesMuted (n.song, both);
    CHECK (phazite::areAllNotesMuted (n.song, both));
    phazite::toggleNotesMuted (n.song, both);
    CHECK_FALSE (n.get (low).muted);

    phazite::setNoteVelocities (n.song, both, 200);
    CHECK (n.get (high).velocity == 127);
}

TEST_CASE ("Duplicated notes go right after the selection")
{
    Notes n;
    const auto a = n.add (60, 4.0, 1.0);
    const auto b = n.add (64, 5.0, 0.5);
    const auto copies = phazite::duplicateNotes (n.song, { a, b });

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
        const auto note = n.add (60, 8.5, 1.0);
        CHECK (n.startOf (note) == doctest::Approx (8.5));
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (8.0));
        CHECK (n.rangeOf (next).getStart().inBeats() == doctest::Approx (8.0));
        CHECK (n.numNotesIn (next) == 0);
    }

    SUBCASE ("A clip before the bar's end: it grows to meet it, not past it")
    {
        const auto next = addClip (10.0, 14.0);
        const auto note = n.add (60, 9.0, 0.5);
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (10.0)); // not 12, where the bar ends
        phazite::moveNote (n.song, note, beats (13.0), 60);
        phazite::setNoteLength (n.song, note, length (4.0));
        CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (10.0));
        CHECK (n.rangeOf (next).getStart().inBeats() == doctest::Approx (10.0));
        CHECK (n.rangeOf (next).getEnd().inBeats() == doctest::Approx (14.0));
    }

    SUBCASE ("A clip that already reaches past its end: it doesn't grow")
    {
        addClip (6.0, 10.0);
        n.add (60, 8.5, 0.5);
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
        n.add (60, 9.0, 0.5);
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

    REQUIRE (n.registry.invoke ("edit.duplicate"));
    CHECK (n.song.getUndoManager().getUndoDescription() == juce::String ("Duplicate 2 Notes"));

    const auto copies = phazite::getSelectedNotes (n.selection);
    REQUIRE (copies.size() == 2);
    CHECK (n.startOf (copies[0]) == doctest::Approx (7.5));
    CHECK (n.startOf (copies[1]) == doctest::Approx (8.5));
    CHECK (n.rangeOf (n.clip).getEnd().inBeats() == doctest::Approx (10.0)); // up to the next clip, not to beat 12
    CHECK (n.rangeOf (next).getStart().inBeats() == doctest::Approx (10.0));

    n.selection.deselectAll(); // the copies go with the undo
    phazite::undo (n.song);
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

    CHECK_FALSE (n.registry.find ("note.transposeUp")->isEnabled());
    n.select ({ a, b });
    CHECK (phazite::getSelectedNotes (n.selection).size() == 2);

    REQUIRE (n.registry.invoke ("note.octaveUp"));
    CHECK (n.get (a).pitch == 72);
    CHECK (undoManager.getUndoDescription() == juce::String ("Octave Up"));

    REQUIRE (n.registry.invoke ("note.lengthen"));
    CHECK (n.get (b).length.inBeats() == doctest::Approx (1.25)); // a step of the grid

    // Legato replaces the notes and selects the new ones.
    REQUIRE (n.registry.invoke ("note.legato"));
    auto selected = phazite::getSelectedNotes (n.selection);
    REQUIRE (selected.size() == 2);
    CHECK (n.get (selected[0]).length.inBeats() == doctest::Approx (1.0));

    REQUIRE (n.registry.invoke ("note.chop"));
    CHECK (n.numNotesIn (n.clip) == 4 + 5); // 1 beat in 4 pieces, 1.25 beats in 5
    CHECK (phazite::getSelectedNotes (n.selection).size() == 9);

    // The up arrow runs the note command while notes are selected, and nothing once none are. (ShortcutTests checks
    // that a note command wins over a track command on the same key.)
    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["note.transposeUp"] = { "cursor up" };
    phazite::ui::CommandBridge commands { n.registry };
    commands.setKeymap (keymap);
    const phazite::ui::KeyDispatcher keys { commands };
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

    // With no notes selected, nothing changes.
    CHECK_FALSE (phazite::hasSelectedNotes (n.selection));
    CHECK (phazite::replaceSelectedNotes (n.selection, octaveUp).empty());
    CHECK (n.numNotesIn (n.clip) == 2);

    n.select ({ a, b });
    CHECK (phazite::hasSelectedNotes (n.selection));
    const auto added = phazite::replaceSelectedNotes (n.selection, octaveUp);
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

    phazite::chooseTrackKey (song, track, phazite::Key { 9, phazite::Scale::minor });
    CHECK (undo.getUndoDescription() == juce::String ("Set Key"));
    phazite::chooseTrackKey (song, track, std::nullopt);
    CHECK (undo.getUndoDescription() == juce::String ("Turn Key Off"));

    phazite::changeNoteVelocities (song, { note }, -10);
    CHECK (noteOf (song, note).velocity == 90);
    CHECK (undo.getUndoDescription() == juce::String ("Change Velocity"));

    auto data = phazite::getNoteData (song, std::vector { note });
    data.front().pitch = 67;
    const auto replaced = phazite::replaceNotes (song, { note }, data, "Transpose Notes");
    REQUIRE (replaced.size() == 1);
    CHECK (noteOf (song, replaced.front()).pitch == 67);
    CHECK (undo.getUndoDescription() == juce::String ("Transpose Notes"));
}

TEST_CASE ("Slide and portamento notes are switched by their commands, each one undo step, and their copies keep them")
{
    Notes n;
    const auto c = n.add (60, 4.0, 2.0);
    const auto e = n.add (64, 5.0, 1.0);
    n.select ({ e });
    CHECK_FALSE (n.registry.find ("note.slide")->isTicked());

    // A slide note, saved on the song's note as well as shown.
    REQUIRE (n.registry.invoke ("note.slide"));
    CHECK (n.registry.find ("note.slide")->isTicked());
    CHECK (n.get (e).slide);
    CHECK_FALSE (n.get (e).portamento);
    CHECK_FALSE (n.get (c).slide);
    CHECK (phazite::describeUndo (&n.song) == juce::String ("Undo Slide Note"));

    // Portamento on both; with one of them already, it goes on for the other.
    n.select ({ c, e });
    REQUIRE (n.registry.invoke ("note.portamento"));
    CHECK (n.registry.find ("note.portamento")->isTicked());
    CHECK (n.get (c).portamento);
    CHECK_FALSE (n.registry.find ("note.slide")->isTicked()); // not every selected note is a slide note

    // Copies keep them.
    n.select ({ e });
    REQUIRE (n.registry.invoke ("edit.duplicate"));
    auto copies = 0;

    for (const auto& data : phazite::getClipNotes (n.song, n.clip))
        if (data.pitch == 64 && data.slide && data.portamento)
            ++copies;

    CHECK (copies == 2);

    // Undone, step by step.
    phazite::undo (n.song);
    phazite::undo (n.song);
    CHECK_FALSE (n.get (c).portamento);
    phazite::undo (n.song);
    CHECK_FALSE (n.get (e).slide);
}
