// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/Clips.h"
#include "model/Commands.h"
#include "model/Notes.h"
#include "model/Selection.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

te::BeatDuration length (double b)
{
    return te::BeatDuration::fromBeats (b);
}

// An Edit with a MIDI clip from beat 4 to 8 (one bar, 4/4), the note selection for it, and the selection
// and note commands, with a grid of a quarter beat.
struct Notes
{
    Notes()
    {
        auto* track = te::getAudioTracks (*edit)[0];
        clip = phazite::insertMidiClip (*track, { beats (4), beats (8) }, nullptr);
        REQUIRE (clip != nullptr);
        events = std::make_unique<te::SelectedMidiEvents> (*clip);
        selection.edit = edit.get();
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addNoteCommands (registry, selection, [] { return 0.25; });
        edit->getUndoManager().clearUndoHistory();
    }

    ~Notes()
    {
        selection.deselectAll();
        events = nullptr;
    }

    te::MidiNote& add (int pitch, double start, double noteLength, int velocity = 100)
    {
        auto* note = phazite::addNote (*clip, pitch, beats (start), length (noteLength), velocity);
        REQUIRE (note != nullptr);
        return *note;
    }

    void select (std::initializer_list<te::MidiNote*> notes) { events->setSelected (selection, juce::Array<te::MidiNote*> (notes), false); }

    double startOf (const te::MidiNote& note) const { return phazite::getNoteStart (*clip, note).inBeats(); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::MidiClip* clip = nullptr;
    std::unique_ptr<te::SelectedMidiEvents> events;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("Notes are placed in song beats, and their clip grows to hold them")
{
    Notes n;
    auto& note = n.add (60, 5.0, 1.0);
    CHECK (n.startOf (note) == doctest::Approx (5.0));
    CHECK (note.getStartBeat().inBeats() == doctest::Approx (1.0)); // in the clip, from its content's start

    // A note can't start before its clip, and one past its end makes it grow to the end of that bar.
    auto& early = n.add (62, 2.0, 1.0);
    CHECK (n.startOf (early) == doctest::Approx (4.0));

    n.add (64, 9.0, 0.5);
    CHECK (n.clip->getEndBeat().inBeats() == doctest::Approx (12.0));

    phazite::moveNote (*n.clip, note, beats (13.5), 72);
    CHECK (n.startOf (note) == doctest::Approx (13.5));
    CHECK (note.getNoteNumber() == 72);
    CHECK (n.clip->getEndBeat().inBeats() == doctest::Approx (16.0));

    phazite::setNoteLength (*n.clip, note, length (0.0));
    CHECK (note.getLengthBeats() == phazite::getMinimumNoteLength());
}

TEST_CASE ("Transposing and nudging keep the notes together, within their limits")
{
    Notes n;
    auto& low = n.add (2, 4.5, 1.0);
    auto& high = n.add (60, 5.0, 1.0);
    const std::vector<phazite::ClipNote> both { { n.clip, &low }, { n.clip, &high } };

    // The lowest note can only go down 2, so the chord goes down 2.
    phazite::transposeNotes (both, -12);
    CHECK (low.getNoteNumber() == 0);
    CHECK (high.getNoteNumber() == 58);

    // The earliest can only go back half a beat, to the clip's start.
    phazite::nudgeNotes (both, length (-2.0));
    CHECK (n.startOf (low) == doctest::Approx (4.0));
    CHECK (n.startOf (high) == doctest::Approx (4.5));

    phazite::changeNoteLengths (both, length (0.5));
    CHECK (high.getLengthBeats().inBeats() == doctest::Approx (1.5));

    phazite::toggleNotesMuted (both);
    CHECK (phazite::areAllNotesMuted (both));
    phazite::toggleNotesMuted (both);
    CHECK_FALSE (low.isMute());

    phazite::setNoteVelocities (both, 200);
    CHECK (high.getVelocity() == 127);
}

TEST_CASE ("Duplicated notes go right after the selection")
{
    Notes n;
    auto& a = n.add (60, 4.0, 1.0);
    auto& b = n.add (64, 5.0, 0.5);
    const auto copies = phazite::duplicateNotes ({ { n.clip, &a }, { n.clip, &b } });

    REQUIRE (copies.size() == 2);
    CHECK (n.startOf (*copies[0].note) == doctest::Approx (5.5));
    CHECK (n.startOf (*copies[1].note) == doctest::Approx (6.5));
    CHECK (copies[1].note->getNoteNumber() == 64);
    CHECK (n.clip->getSequence().getNumNotes() == 4);
}

TEST_CASE ("Quantize moves notes toward the grid, with strength and swing")
{
    using phazite::NoteData;
    const std::vector<NoteData> notes { { nullptr, 60, beats (0.1), length (0.3), 100, false },
                                        { nullptr, 62, beats (0.6), length (0.3), 100, false } };

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
    std::vector<NoteData> notes { { nullptr, 60, beats (0.0), length (0.25), 100, false },
                                  { nullptr, 64, beats (0.0), length (0.5), 100, false }, // a chord with the first
                                  { nullptr, 67, beats (1.0), length (0.25), 100, false } };

    const auto connected = phazite::legato (notes);
    CHECK (connected[0].length.inBeats() == doctest::Approx (1.0));
    CHECK (connected[1].length.inBeats() == doctest::Approx (1.0));
    CHECK (connected[2].length.inBeats() == doctest::Approx (0.25)); // nothing after it

    CHECK (phazite::legato (notes, 0.25)[0].length.inBeats() == doctest::Approx (0.75));

    const auto pieces = phazite::chop ({ { nullptr, 60, beats (2.0), length (1.1), 90, false } }, 0.25);
    REQUIRE (pieces.size() == 5);
    CHECK (pieces[1].start.inBeats() == doctest::Approx (2.25));
    CHECK (pieces[4].length.inBeats() == doctest::Approx (0.1));
    CHECK (pieces[4].velocity == 90);
}

TEST_CASE ("Keys: shading, snapping and transposing in the key")
{
    const phazite::Key cMajor { 0, te::Scale::major };
    CHECK (phazite::isInKey (60, cMajor));
    CHECK_FALSE (phazite::isInKey (61, cMajor));
    CHECK (phazite::snapToKey (61, cMajor) == 60); // the lower one when both are as near
    CHECK (phazite::snapToKey (66, cMajor) == 65);
    CHECK (phazite::describeKey (cMajor) == juce::String ("C major"));

    const phazite::Key aMinor { 9, te::Scale::minor };
    CHECK (phazite::isInKey (60, aMinor));
    CHECK_FALSE (phazite::isInKey (61, aMinor));

    using phazite::NoteData;
    const std::vector<NoteData> notes { { nullptr, 64, beats (0.0), length (1.0), 100, false } };

    // E up two steps of C major is G; up a semitone, F; down 3 steps, B below.
    CHECK (phazite::transpose (notes, 2, cMajor)[0].pitch == 67);
    CHECK (phazite::transpose (notes, 1, std::nullopt)[0].pitch == 65);
    CHECK (phazite::transpose (notes, -3, cMajor)[0].pitch == 59);
    CHECK (phazite::transpose (notes, 7, cMajor)[0].pitch == 76); // an octave
    CHECK (phazite::transpose ({ { nullptr, 125, beats (0.0), length (1.0), 100, false } }, 12, std::nullopt)[0].pitch == 127);

    // A new Edit is in C major.
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    CHECK (phazite::describeKey (phazite::getKeyAt (*edit, beats (0.0))) == juce::String ("C major"));
}

TEST_CASE ("Chord stamps and drum rows")
{
    CHECK (phazite::getChordPitches (60, te::Chord::majorTriad) == std::vector<int> { 60, 64, 67 });
    CHECK (phazite::getChordPitches (57, te::Chord::minorSeventhChord) == std::vector<int> { 57, 60, 64, 67 });
    CHECK (phazite::getChordPitches (125, te::Chord::majorTriad) == std::vector<int> { 125 }); // the rest are out of range
    CHECK_FALSE (phazite::getChordShapes().empty());

    CHECK (phazite::getDrumName (36) == juce::String ("Kick"));
    CHECK (phazite::getDrumName (42) == juce::String ("Closed Hat"));
    CHECK (phazite::getDrumName (56) == juce::String ("Cowbell"));

    Notes n;
    auto& track = *te::getAudioTracks (*n.edit)[0];
    CHECK_FALSE (phazite::isDrumTrack (track)); // no sampler
    phazite::setDrumTrack (track, true);
    CHECK (phazite::isDrumTrack (track));

    // The usual drums, with the other notes the clip plays after them.
    n.add (70, 4.0, 0.25);
    auto rows = phazite::getDrumRows (track, { n.clip });
    REQUIRE (rows.size() == 12);
    CHECK (rows.front().note == 36);
    CHECK (rows.back().note == 70);

    // The order and the hidden rows are the track's.
    phazite::moveDrumRow (track, rows, 70, 0);
    phazite::hideDrumRow (track, 37);
    rows = phazite::getDrumRows (track, { n.clip });
    CHECK (rows.front().note == 70);
    CHECK (rows.size() == 11);

    phazite::showAllDrumRows (track);
    CHECK (phazite::getDrumRows (track, { n.clip }).size() == 12);
}

TEST_CASE ("The note commands act on the selected notes, and take the arrow keys while notes are selected")
{
    Notes n;
    auto& a = n.add (60, 4.0, 1.0);
    auto& b = n.add (64, 5.0, 1.0);
    auto& undoManager = n.edit->getUndoManager();

    CHECK_FALSE (n.registry.find ("note.transposeUp")->isEnabled());
    n.select ({ &a, &b });
    CHECK (phazite::getSelectedNotes (n.selection).size() == 2);

    REQUIRE (n.registry.invoke ("note.octaveUp"));
    CHECK (a.getNoteNumber() == 72);
    CHECK (undoManager.getUndoDescription() == juce::String ("Octave Up"));

    REQUIRE (n.registry.invoke ("note.lengthen"));
    CHECK (b.getLengthBeats().inBeats() == doctest::Approx (1.25)); // a step of the grid

    // Legato replaces the notes and selects the new ones.
    REQUIRE (n.registry.invoke ("note.legato"));
    auto selected = phazite::getSelectedNotes (n.selection);
    REQUIRE (selected.size() == 2);
    CHECK (selected[0].note->getLengthBeats().inBeats() == doctest::Approx (1.0));

    REQUIRE (n.registry.invoke ("note.chop"));
    CHECK (n.clip->getSequence().getNumNotes() == 4 + 5); // 1 beat in 4 pieces, 1.25 beats in 5
    CHECK (phazite::getSelectedNotes (n.selection).size() == 9);

    // The up arrow runs the note command while notes are selected, and nothing once none are. (CommandTests checks
    // that a note command wins over a track command on the same key.)
    phazite::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["note.transposeUp"] = { "cursor up" };
    n.registry.setKeymap (keymap);
    CHECK (n.registry.findCommandForKey (juce::KeyPress (juce::KeyPress::upKey))->id == juce::String ("note.transposeUp"));

    n.selection.deselectAll();
    CHECK (n.registry.findCommandForKey (juce::KeyPress (juce::KeyPress::upKey)) == nullptr);
}
