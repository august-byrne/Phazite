// SPDX-License-Identifier: AGPL-3.0-or-later
// Chords as plain values (src/model/chords/ChordValues.h): numerals read and written, what they are in a key, the keys
// progressions play in, and the notes of the parts made from chords; and the progressions that ship with Phazite
// (src/model/chords/Progressions.h).
#include "TestHelpers.h"
#include "model/chords/ChordValues.h"
#include "model/chords/Progressions.h"

#include <set>

namespace
{
using phazite::ChordType;
using phazite::Key;
using phazite::Numeral;
using phazite::PartStyle;
using phazite::Scale;

const Key cMajor { 0, Scale::major };
const Key aMinor { 9, Scale::minor };

phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

Numeral parse (const char* text)
{
    const auto numeral = phazite::parseNumeral (text);
    INFO (text);
    REQUIRE (numeral.has_value());
    return *numeral;
}

juce::String nameIn (const char* numeral, const Key& key)
{
    return phazite::getChordNameIn (parse (numeral), key);
}

// Chords a bar long each, in a key, from the song's start.
std::vector<phazite::PlacedChord> bars (std::initializer_list<const char*> numerals, const Key& key = cMajor)
{
    std::vector<phazite::PlacedChord> chords;
    auto start = 0.0;

    for (const auto* numeral : numerals)
    {
        chords.push_back ({ { beat (start), beat (start + 4.0) }, parse (numeral), key });
        start += 4.0;
    }

    return chords;
}

std::vector<phazite::NoteData> notesFrom (double from, double to, const std::vector<phazite::NoteData>& notes)
{
    std::vector<phazite::NoteData> found;

    for (const auto& note : notes)
        if (note.start >= beat (from) && note.start < beat (to))
            found.push_back (note);

    return found;
}

std::set<int> getPitchClasses (const std::vector<phazite::NoteData>& notes)
{
    std::set<int> classes;

    for (const auto& note : notes)
        classes.insert (note.pitch % 12);

    return classes;
}

std::vector<int> getPitches (const std::vector<phazite::NoteData>& notes)
{
    std::vector<int> pitches;

    for (const auto& note : notes)
        pitches.push_back (note.pitch);

    return pitches;
}
} // namespace

TEST_CASE ("Numerals: read as progressions write them, and written back")
{
    CHECK (parse ("I") == Numeral { 1, 0, ChordType::major, 0 });
    CHECK (parse ("vi") == Numeral { 6, 0, ChordType::minor, 0 });
    CHECK (parse ("bVII") == Numeral { 7, -1, ChordType::major, 0 });
    CHECK (parse ("#iv") == Numeral { 4, 1, ChordType::minor, 0 });
    CHECK (parse ("ii7") == Numeral { 2, 0, ChordType::minorSeventh, 0 });
    CHECK (parse ("V7") == Numeral { 5, 0, ChordType::dominantSeventh, 0 });
    CHECK (parse ("IVmaj7") == Numeral { 4, 0, ChordType::majorSeventh, 0 });
    CHECK (parse ("viio") == Numeral { 7, 0, ChordType::diminished, 0 });
    CHECK (parse ("iim7b5") == Numeral { 2, 0, ChordType::halfDiminishedSeventh, 0 });
    CHECK (parse ("viio7") == Numeral { 7, 0, ChordType::diminishedSeventh, 0 });
    CHECK (parse ("I+") == Numeral { 1, 0, ChordType::augmented, 0 });
    CHECK (parse ("Vsus4") == Numeral { 5, 0, ChordType::suspendedFourth, 0 });
    CHECK (parse ("I5") == Numeral { 1, 0, ChordType::power, 0 });
    CHECK (parse ("ii9") == Numeral { 2, 0, ChordType::minorNinth, 0 });
    CHECK (parse ("V/1") == Numeral { 5, 0, ChordType::major, 1 });
    CHECK (parse ("V7/3") == Numeral { 5, 0, ChordType::dominantSeventh, 3 });

    // Not numerals: mixed case, a step that isn't one, an unknown kind, a minor chord written as a major one's kind, and
    // an inversion a chord doesn't have.
    for (const auto* text : { "", "Vi", "VIII", "X", "I7b9", "imaj7", "I/3", "I/x", "bb" })
    {
        INFO (text);
        CHECK_FALSE (phazite::parseNumeral (text).has_value());
    }

    // Written back with the signs for diminished and half-diminished chords, which read again as the same chords.
    const auto degree = juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
    const auto slashedO = juce::String (juce::CharPointer_UTF8 ("\xc3\xb8"));
    CHECK (phazite::describeNumeral (parse ("bVII")) == juce::String ("bVII"));
    CHECK (phazite::describeNumeral (parse ("ii7")) == juce::String ("ii7"));
    CHECK (phazite::describeNumeral (parse ("V/1")) == juce::String ("V/1"));
    CHECK (phazite::describeNumeral (parse ("viio")) == "vii" + degree);
    CHECK (phazite::describeNumeral (parse ("iim7b5")) == "ii" + slashedO + "7");

    for (const auto* text : { "I", "vi", "bVII", "ii7", "V7", "IVmaj7", "viio", "iim7b5", "viio7", "I+", "Vsus2", "I6", "vi6", "V9",
                              "Imaj9", "ii9", "IV/2", "I5" })
    {
        INFO (text);
        CHECK (phazite::parseNumeral (phazite::describeNumeral (parse (text))) == parse (text));
    }
}

TEST_CASE ("Numerals in a key: roots, notes and names, with the key's flats or sharps")
{
    CHECK (nameIn ("I", cMajor) == juce::String ("C"));
    CHECK (nameIn ("vi", cMajor) == juce::String ("Am"));
    CHECK (nameIn ("bVII", cMajor) == juce::String ("Bb"));
    CHECK (nameIn ("#iv", cMajor) == juce::String ("F#m"));
    CHECK (nameIn ("ii7", cMajor) == juce::String ("Dm7"));
    CHECK (nameIn ("viio", cMajor) == juce::String ("Bdim"));
    CHECK (nameIn ("V/1", cMajor) == juce::String ("G/B"));
    CHECK (nameIn ("IVmaj7", cMajor) == juce::String ("Fmaj7"));
    CHECK (nameIn ("iim7b5", aMinor) == juce::String ("Bm7b5"));

    CHECK (nameIn ("IV", { 5, Scale::major }) == juce::String ("Bb"));   // F major has a flat
    CHECK (nameIn ("iii", { 4, Scale::major }) == juce::String ("G#m")); // E major has sharps
    CHECK (nameIn ("VI", { 2, Scale::minor }) == juce::String ("Bb"));   // D minor has a flat
    CHECK (nameIn ("i", aMinor) == juce::String ("Am"));
    CHECK (nameIn ("VII", aMinor) == juce::String ("G"));

    CHECK (phazite::getRootIn (parse ("bVII"), cMajor) == 10);
    CHECK (phazite::getPitchClassesIn (parse ("I"), cMajor) == std::vector<int> { 0, 4, 7 });
    CHECK (phazite::getPitchClassesIn (parse ("V7/1"), cMajor) == std::vector<int> { 11, 2, 5, 7 });

    // Back from a root and a kind: on a step of the scale, or a lowered step.
    CHECK (phazite::toNumeral (10, ChordType::major, 0, cMajor) == parse ("bVII"));
    CHECK (phazite::toNumeral (9, ChordType::minor, 0, cMajor) == parse ("vi"));
    CHECK (phazite::toNumeral (4, ChordType::major, 0, aMinor) == parse ("V"));
    CHECK (phazite::toNumeral (10, ChordType::major, 0, aMinor) == parse ("bII"));
}

TEST_CASE ("The chords of a key's scale, and the key a progression plays in")
{
    const auto describeAll = [] (const Key& key)
    {
        juce::StringArray names;

        for (const auto& numeral : phazite::getDiatonicChords (key))
            names.add (phazite::getChordNameIn (numeral, key));

        return names.joinIntoString (" ");
    };

    CHECK (describeAll (cMajor) == juce::String ("C Dm Em F G Am Bdim"));
    CHECK (describeAll (aMinor) == juce::String ("Am Bdim C Dm Em F G"));
    CHECK (describeAll ({ 9, Scale::harmonicMinor }) == juce::String ("Am Bdim Caug Dm E F G#dim"));
    CHECK (describeAll ({ 2, Scale::dorian }) == juce::String ("Dm Em F G Am Bdim C"));

    CHECK (phazite::hasMajorThird (cMajor));
    CHECK (phazite::hasMajorThird ({ 7, Scale::mixolydian }));
    CHECK_FALSE (phazite::hasMajorThird (aMinor));

    // The same third: the song's key's root. Another: the key with the song's key's notes.
    CHECK (phazite::getKeyForProgression (cMajor, Scale::major) == cMajor);
    CHECK (phazite::getKeyForProgression (aMinor, Scale::minor) == aMinor);
    CHECK (phazite::getKeyForProgression (cMajor, Scale::minor) == aMinor);
    CHECK (phazite::getKeyForProgression (aMinor, Scale::major) == cMajor);
    CHECK (phazite::getKeyForProgression ({ 2, Scale::dorian }, Scale::major) == cMajor);
    CHECK (phazite::getKeyForProgression ({ 7, Scale::mixolydian }, Scale::minor) == aMinor);
}

TEST_CASE ("Chord kinds and part styles keep the names projects save them by")
{
    for (const auto& shape : phazite::getChordShapes())
    {
        const auto saved = phazite::getSavedChordTypeName (shape.type);
        INFO (shape.name);
        CHECK (saved.isNotEmpty());
        CHECK (phazite::findChordType (saved) == shape.type);
    }

    CHECK (phazite::getSavedChordTypeName (ChordType::dominantSeventh) == juce::String ("dominantSeventh"));
    CHECK (phazite::getSavedChordTypeName (ChordType::halfDiminishedSeventh) == juce::String ("halfDiminishedSeventh"));
    CHECK_FALSE (phazite::findChordType ("seventh").has_value());

    CHECK (phazite::getPartStyles().size() == 12);

    for (const auto style : phazite::getPartStyles())
    {
        const auto saved = phazite::getSavedPartStyleName (style);
        CHECK (phazite::findPartStyle (saved) == style);
        CHECK (phazite::getPartStyleName (style).isNotEmpty());
    }

    CHECK (phazite::getSavedPartStyleName (PartStyle::bassRootFifth) == juce::String ("bassRootFifth"));
    CHECK (phazite::getPartKind (PartStyle::offbeatStabs) == phazite::PartKind::chords);
    CHECK (phazite::getPartKind (PartStyle::bassOctaves) == phazite::PartKind::bass);
    CHECK (phazite::getPartKind (PartStyle::arpeggioUpDown) == phazite::PartKind::arpeggio);
}

TEST_CASE ("Parts made from chords: held chords that move little, around middle C")
{
    const phazite::ClipID clip = phazite::ClipID::fromRaw (7);
    const auto chords = bars ({ "I", "vi", "IV", "V" });
    const auto notes = phazite::makePartNotes (chords, PartStyle::sustainedTriads, { beat (0.0), beat (16.0) }, clip);
    REQUIRE (notes.size() == 12);

    const std::set<int> expected[] = { { 0, 4, 7 }, { 9, 0, 4 }, { 5, 9, 0 }, { 7, 11, 2 } };
    std::vector<int> previous;

    for (int bar = 0; bar < 4; ++bar)
    {
        const auto chord = notesFrom (bar * 4.0, bar * 4.0 + 4.0, notes);
        REQUIRE (chord.size() == 3);
        CHECK (getPitchClasses (chord) == expected[bar]);

        for (const auto& note : chord)
        {
            CHECK (note.clip == clip);
            CHECK (note.length.inBeats() == doctest::Approx (4.0));
            CHECK (note.pitch >= 48);
            CHECK (note.pitch <= 76);
        }

        // Each voice moves at most a few semitones to the next chord.
        auto pitches = getPitches (chord);
        std::sort (pitches.begin(), pitches.end());

        if (! previous.empty())
            for (size_t i = 0; i < pitches.size(); ++i)
                CHECK (std::abs (pitches[i] - previous[i]) <= 4);

        previous = pitches;
    }

    // Sevenths add the scale's seventh above each triad: Cmaj7 in C major.
    const auto sevenths = phazite::makePartNotes (bars ({ "I" }), PartStyle::sustainedSevenths, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitchClasses (sevenths) == std::set<int> { 0, 4, 7, 11 });

    // Spread chords: the root low, with its fifth, and the rest an octave above (the fifth again among them).
    const auto spread = getPitches (phazite::makePartNotes (bars ({ "I" }), PartStyle::spreadChords, { beat (0.0), beat (4.0) }, clip));
    CHECK (spread == std::vector<int> { 48, 55, 64, 67 });
}

TEST_CASE ("Parts made from chords: rhythms, bass lines and arpeggios, cut at the range's edges")
{
    const phazite::ClipID clip = phazite::ClipID::fromRaw (7);

    const auto pulses = phazite::makePartNotes (bars ({ "I" }), PartStyle::pulsingChords, { beat (0.0), beat (4.0) }, clip);
    CHECK (pulses.size() == 12);
    CHECK (notesFrom (1.0, 1.1, pulses).size() == 3);

    const auto stabs = phazite::makePartNotes (bars ({ "I" }), PartStyle::offbeatStabs, { beat (0.0), beat (4.0) }, clip);
    REQUIRE (stabs.size() == 12);
    CHECK (stabs[0].start.inBeats() == doctest::Approx (0.5));
    CHECK (stabs[0].length.inBeats() == doctest::Approx (0.25));

    // Bass: the root from C2 up, or an inversion's lowest note.
    const auto roots = phazite::makePartNotes (bars ({ "I", "vi", "IV", "V/1" }), PartStyle::bassRoots, { beat (0.0), beat (16.0) }, clip);
    CHECK (getPitches (roots) == std::vector<int> { 36, 45, 41, 47 });

    const auto rootFifth = phazite::makePartNotes (bars ({ "vi" }), PartStyle::bassRootFifth, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (rootFifth) == std::vector<int> { 45, 52, 45, 52 });

    // The chord's own fifth: diminished over a half-diminished chord, and the octave over a chord with its fifth in the bass.
    const auto halfDiminished = phazite::makePartNotes (bars ({ "viim7b5" }), PartStyle::bassRootFifth, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (halfDiminished) == std::vector<int> { 47, 53, 47, 53 });
    const auto fifthBelow = phazite::makePartNotes (bars ({ "I/2" }), PartStyle::bassRootFifth, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (fifthBelow) == std::vector<int> { 43, 55, 43, 55 });

    const auto octaves = phazite::makePartNotes (bars ({ "I" }), PartStyle::bassOctaves, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (octaves) == std::vector<int> { 36, 48, 36, 48, 36, 48, 36, 48 });

    const auto eighths = phazite::makePartNotes (bars ({ "I" }), PartStyle::bassPulses, { beat (0.0), beat (4.0) }, clip);
    CHECK (eighths.size() == 8);

    // Arpeggios: eighth notes from middle C, with the octave at the top.
    const auto up = phazite::makePartNotes (bars ({ "I" }), PartStyle::arpeggioUp, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (up) == std::vector<int> { 60, 64, 67, 72, 60, 64, 67, 72 });

    const auto down = phazite::makePartNotes (bars ({ "I" }), PartStyle::arpeggioDown, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (down) == std::vector<int> { 72, 67, 64, 60, 72, 67, 64, 60 });

    const auto upDown = phazite::makePartNotes (bars ({ "I" }), PartStyle::arpeggioUpDown, { beat (0.0), beat (4.0) }, clip);
    CHECK (getPitches (upDown) == std::vector<int> { 60, 64, 67, 72, 67, 64, 60, 64 });

    // A range that starts and ends inside chords: held notes are cut to it, and steps before it are left out.
    const auto cut = phazite::makePartNotes (bars ({ "I", "vi" }), PartStyle::sustainedTriads, { beat (2.0), beat (6.0) }, clip);
    REQUIRE (cut.size() == 6);

    for (const auto& note : cut)
    {
        CHECK (note.start >= beat (2.0));
        CHECK (note.getEnd() <= beat (6.0));
        CHECK (note.length.inBeats() == doctest::Approx (2.0));
    }

    const auto cutPulses = phazite::makePartNotes (bars ({ "I" }), PartStyle::bassRootFifth, { beat (1.5), beat (4.0) }, clip);
    CHECK (cutPulses.size() == 2);
}

TEST_CASE ("The progressions that ship with Phazite")
{
    const auto& progressions = phazite::getBuiltInProgressions();
    CHECK (progressions.size() == 40);

    std::set<juce::String> ids;

    for (const auto& progression : progressions)
    {
        INFO (progression.id);
        CHECK (ids.insert (progression.id).second);
        CHECK (progression.name.isNotEmpty());
        CHECK (progression.feel.isNotEmpty());
        CHECK_FALSE (progression.moods.isEmpty());
        CHECK_FALSE (progression.genres.isEmpty());
        CHECK_FALSE (progression.chords.empty());
        CHECK ((progression.scale == Scale::major || progression.scale == Scale::minor));

        // Whole bars, so that a progression repeats on the bar.
        CHECK (std::fmod (progression.getLengthInBars(), 1.0) == doctest::Approx (0.0));
    }

    // The design's starting set (docs/design.md, section 14) is there.
    for (const auto* id : { "popAnthem", "wistfulPop", "doowop", "jazzTwoFiveOne", "epicMinor", "drivingMinor", "darkMinor",
                            "threeChordRock", "mixolydianRock", "royalRoad", "andalusian", "twelveBarBlues" })
    {
        INFO (id);
        CHECK (phazite::findProgression (id) != nullptr);
    }

    const auto* anthem = phazite::findProgression ("popAnthem");
    REQUIRE (anthem != nullptr);
    CHECK (anthem->describeNumerals() == juce::String ("I - V - vi - IV"));
    CHECK (phazite::getChordNamesIn (*anthem, cMajor).joinIntoString (" ") == juce::String ("C G Am F"));
    CHECK (phazite::getChordNamesIn (*anthem, { 2, Scale::major }).joinIntoString (" ") == juce::String ("D A Bm G"));

    // A minor progression in a major key plays in its relative minor, and the other way round.
    const auto* epic = phazite::findProgression ("epicMinor");
    REQUIRE (epic != nullptr);
    CHECK (phazite::getChordNamesIn (*epic, cMajor).joinIntoString (" ") == juce::String ("Am F C G"));
    CHECK (phazite::getChordNamesIn (*anthem, aMinor).joinIntoString (" ") == juce::String ("C G Am F"));

    const auto* blues = phazite::findProgression ("twelveBarBlues");
    REQUIRE (blues != nullptr);
    CHECK (blues->getLengthInBars() == doctest::Approx (12.0));

    // Moods and genres, each once, and the progressions with them.
    CHECK (phazite::getProgressionMoods().contains ("dark"));
    CHECK (phazite::getProgressionGenres().contains ("jazz"));

    const auto dark = phazite::findProgressions ("dark", {});
    CHECK (std::find (dark.begin(), dark.end(), phazite::findProgression ("darkMinor")) != dark.end());
    CHECK (std::find (dark.begin(), dark.end(), anthem) == dark.end());

    const auto darkTrap = phazite::findProgressions ("dark", "trap");
    CHECK_FALSE (darkTrap.empty());

    for (const auto* progression : darkTrap)
        CHECK ((progression->moods.contains ("dark") && progression->genres.contains ("trap")));

    CHECK (phazite::findProgressions ({}, {}).size() == progressions.size());

    // A progression's entry: numerals with lengths in bars, and nothing for one with a chord that isn't a numeral.
    const auto entry = juce::JSON::parse (R"({ "id": "test", "name": "Test", "scale": "minor", "chords": "i*2 VII*0.5 v7*1.5" })");
    const auto read = phazite::Progression::fromJSON (entry);
    REQUIRE (read.has_value());
    CHECK (read->scale == Scale::minor);
    REQUIRE (read->chords.size() == 3);
    CHECK (read->chords[1].bars == doctest::Approx (0.5));
    CHECK (read->chords[2].numeral == parse ("v7"));
    CHECK (read->getLengthInBars() == doctest::Approx (4.0));

    CHECK_FALSE (phazite::Progression::fromJSON (juce::JSON::parse (R"({ "id": "x", "name": "X", "chords": "I Q" })")).has_value());
    CHECK_FALSE (phazite::Progression::fromJSON (juce::JSON::parse (R"({ "id": "x", "name": "X", "chords": "" })")).has_value());
    CHECK_FALSE (phazite::Progression::fromJSON (juce::JSON::parse (R"({ "name": "X", "chords": "I" })")).has_value());
}
