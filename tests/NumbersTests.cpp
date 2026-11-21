// SPDX-License-Identifier: AGPL-3.0-or-later
// Whole numbers made of numbers that may be no finite number (src/model/base/Numbers.h; known bug 148): a cast to an integer
// of a NaN, an infinity or a number past the integer's range is undefined behaviour, which UndefinedBehaviorSanitizer
// (linux-asan) reports where these tests reach one; and what a file or a value typed brings there.
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "model/arrangement/GridSettings.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Json.h"
#include "model/base/Numbers.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/StartupSound.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TempoValues.h"

#include <cmath>
#include <limits>

TEST_CASE ("A number that is no finite number, or past an integer's range, is made a whole one without a cast that is undefined")
{
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto inf = std::numeric_limits<double>::infinity();
    using phazite::roundToNearestInt, phazite::roundToWhole, phazite::toWhole;

    // As the conversions they stand for, within the range.
    CHECK (toWhole<int> (-2.75) == -2);
    CHECK (roundToWhole<int> (2.5) == 3); // halves away from 0, as std::lround
    CHECK (roundToWhole<int> (-2.5) == -3);
    CHECK (roundToNearestInt (2.5) == juce::roundToInt (2.5)); // as juce::roundToInt
    CHECK (roundToNearestInt (3.5f) == juce::roundToInt (3.5f));
    CHECK (roundToWhole<std::int64_t> (4.0e15) == (std::int64_t) 4000000000000000);

    // No number: what it is told; an infinity or a number past the range: the type's end.
    CHECK (toWhole<int> (nan, 7) == 7);
    CHECK (roundToWhole<int> (nan) == 0);
    CHECK (roundToNearestInt (nan, 100) == 100);
    CHECK (toWhole<int> (inf) == std::numeric_limits<int>::max());
    CHECK (toWhole<int> (-inf) == std::numeric_limits<int>::lowest());
    CHECK (roundToWhole<int> (1.0e300) == std::numeric_limits<int>::max());
    CHECK (roundToNearestInt (-1.0e30) == std::numeric_limits<int>::lowest());
    CHECK (roundToWhole<std::int64_t> (9.3e18) == std::numeric_limits<std::int64_t>::max());
    CHECK (roundToWhole<std::int64_t> (-9.3e18) == std::numeric_limits<std::int64_t>::lowest());
    CHECK (toWhole<std::uint32_t> (-1.0) == 0u);

    // A juce::var's double, as JSON's "1e300" or "nan" makes one, and its other numbers as JUCE reads them.
    CHECK (toWhole<juce::int64> (juce::var (1.0e300)) == std::numeric_limits<juce::int64>::max());
    CHECK (toWhole<int> (juce::var (nan), -1) == -1);
    CHECK (toWhole<int> (juce::var (12)) == 12);
    CHECK (toWhole<int> (juce::var ("34")) == 34);

    // A value typed, and a float read from text past a float's range.
    CHECK_FALSE (phazite::keepFinite (inf).has_value());
    CHECK (phazite::keepFinite (1.5) == 1.5);
    CHECK (phazite::readFloat ("1e300") == std::numeric_limits<float>::max());
    CHECK (phazite::readFloat ("-1e300") == std::numeric_limits<float>::lowest());
    CHECK (phazite::readFloat ("0.25") == 0.25f);
    CHECK (phazite::readDouble ("1e12345678901") == 0.0); // never what isn't finite: what it is told, or 0
    CHECK (phazite::readDouble ("-1e999", -1.0) == -1.0);
    CHECK (phazite::readFloat ("inf", 2.0f) == 2.0f);
    CHECK_FALSE (phazite::readFiniteDouble ("1e999").has_value());
    CHECK (phazite::readFiniteDouble (" 2.5") == 2.5);

    // A float of a double: within a float's range, what it is told for what isn't finite; of a var too (a preset's JSON).
    CHECK (phazite::toFloat (1.0e300) == std::numeric_limits<float>::max());
    CHECK (phazite::toFloat (nan, 0.5f) == 0.5f);
    CHECK (phazite::toFloat (juce::var (-1.0e300)) == std::numeric_limits<float>::lowest());
    CHECK (phazite::toFloat (juce::var ("1e12345678901"), 3.0f) == 3.0f);
    CHECK (phazite::toFloat (juce::var (0.25)) == 0.25f);

    // A var's 64-bit whole number past an int's range held at its end, not cut to its lower bits (5e9 to 705032704).
    CHECK (toWhole<int> (juce::var ((juce::int64) 5000000000)) == std::numeric_limits<int>::max());
    CHECK (toWhole<int> (juce::var ((juce::int64) -5000000000)) == std::numeric_limits<int>::lowest());
    CHECK (toWhole<int> (juce::var ((juce::int64) -5)) == -5);
    CHECK (toWhole<std::uint32_t> (juce::var ((juce::int64) -5)) == 0u);
}

TEST_CASE ("A MIDI file timing its notes past any song's end imports them within a song's beats, which bars can count")
{
    // A file from others: each event as long after the last as MIDI's lengths can say, a hundred times over, a quarter note a
    // tick. The beats came in unclamped, ten billion along, past what an int counts in bars (Bars::getBarAt()).
    // Its bytes written here: juce::MidiFile's writer counts ticks in an int, which such times overflow (UBSan, CI,
    // 2026-11-17).
    juce::MemoryOutputStream events;

    for (int i = 0; i < 100; ++i)
    {
        const unsigned char controller[] = { 0xff, 0xff, 0xff, 0x7f, 0xb0, 1, (unsigned char) i }; // 0x0fffffff ticks on
        events.write (controller, sizeof (controller));
    }

    const unsigned char played[] = { 0x00, 0x90, 60, 102, 0x01, 0x80, 60, 0, 0x00, 0xff, 0x2f, 0x00 }; // on, off a tick on, the end
    events.write (played, sizeof (played));

    phazite::test::TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Far.mid");
    {
        juce::FileOutputStream stream (file);
        REQUIRE (stream.openedOk());
        stream.write ("MThd", 4);
        stream.writeIntBigEndian (6);
        stream.writeShortBigEndian (0); // one track
        stream.writeShortBigEndian (1);
        stream.writeShortBigEndian (1); // a quarter note a tick
        stream.write ("MTrk", 4);
        stream.writeIntBigEndian ((int) events.getDataSize());
        REQUIRE (stream.write (events.getData(), events.getDataSize()));
    }

    auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song).front();
    phazite::UndoStep step (*song, "Import");
    const auto clip = phazite::importFile (*song, track, file, phazite::BeatPosition());
    REQUIRE (clip.isValid());

    for (const auto& note : phazite::getClipNotes (*song, clip))
        CHECK (note.getEnd().inBeats() <= phazite::maxSongBeats + 1.0);

    // Its bar, counted.
    const auto end = phazite::getClipRange (*song, clip).getEnd();
    CHECK (end.inBeats() <= phazite::maxSongBeats + 8.0);
    const phazite::Bars bars;
    CHECK (bars.getBarAt (end) >= 0);
    CHECK (bars.getBarAt (phazite::BeatPosition::fromBeats (1.0e300)) == phazite::Bars::maxBars); // counted on from without overflowing
    CHECK (bars.getBarAt (phazite::BeatPosition::fromBeats (-1.0e300)) == -phazite::Bars::maxBars);
    CHECK (std::isfinite (bars.getBarStart (std::numeric_limits<int>::lowest()).inBeats()));

    // Snapped to the coarsest grid there, 1,024 bars a step: to the last step within the bars (its steps were held to the
    // bars' count, then made bars, which overflowed an int).
    const auto snapped = phazite::snapToGrid (phazite::BeatPosition::fromBeats (1.0e300), { phazite::GridSettings::maxLevel, false }, bars);
    CHECK (snapped.inBeats() <= bars.getBarStart (phazite::Bars::maxBars).inBeats());
    CHECK (snapped.inBeats() >= bars.getBarStart (phazite::Bars::maxBars - 1024).inBeats());

    // And the startup sound at a rate no device plays at: none, rather than two billion samples.
    for (const auto rate : { std::numeric_limits<double>::infinity(), 1.0e300, 0.0, -48000.0 })
        CHECK (phazite::makeStartupSound (rate).getNumSamples() == 0);

    CHECK (phazite::makeStartupSound (48000.0).getNumSamples() > 0);
}

TEST_CASE ("A note's value typed that is no finite number changes nothing")
{
    phazite::test::TestSong s;
    const auto clip = s.addMidiClip (s.firstTrack, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0, 90);
    auto& song = *phazite::findSong (s.context);
    const auto refs = phazite::getRefs (phazite::getClipNotes (song, clip));
    REQUIRE (refs.size() == 1);

    for (const auto property : { phazite::NoteProperty::velocity, phazite::NoteProperty::pitch, phazite::NoteProperty::start,
                                 phazite::NoteProperty::length, phazite::NoteProperty::channel })
    {
        phazite::setNotesTo (s.context, refs, property, std::numeric_limits<double>::infinity()); // as typed "1e999" reads
        phazite::changeNotes (s.context, refs, property, std::numeric_limits<double>::quiet_NaN());
    }

    const auto note = phazite::getClipNotes (song, clip).front();
    CHECK (note.velocity == 90);
    CHECK (note.pitch == 60);
    CHECK (note.start.inBeats() == doctest::Approx (0.0));
    CHECK (note.length.inBeats() == doctest::Approx (1.0));
    CHECK (note.channel == 1);
}
