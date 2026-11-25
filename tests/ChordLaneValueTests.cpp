// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's chord lane as a value (model/base/internal/ChordLaneValue.h): the conversion between the value and the CHORDLANE
// element (the one place that reads and writes CHORD and CHORDKEY by name), round trip through the lane of the golden project
// and through damaged ones; and the lane's readers (model/chords/internal/Chords.h) on the value, the same as reading the
// elements the loader leaves.
//   phazite_tests.exe --test-case="Chord lane value*"
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongState.h"
#include "model/chords/ChordValues.h"
#include "model/chords/Progressions.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/NoteValues.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::ChordEntry;
using phazite::ChordLaneValue;
using phazite::KeyEntry;

phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

juce::ValueTree writeLane (const ChordLaneValue& value)
{
    juce::ValueTree written (schema::CHORDLANE);
    phazite::writeChordLane (value, written);
    return written;
}

// An element as XML, each attribute that is a number written as the number (a file's "16" and the value's "16.0" alike), for
// findDifferences() (GoldenFiles.h), which compares text.
std::unique_ptr<juce::XmlElement> toComparable (const juce::ValueTree& element)
{
    auto xml = element.createXml();
    REQUIRE (xml != nullptr);

    const std::function<void (juce::XmlElement&)> normalise = [&normalise] (juce::XmlElement& each)
    {
        for (int i = 0; i < each.getNumAttributes(); ++i)
            if (const auto text = each.getAttributeValue (i);
                text.isNotEmpty() && text.containsOnly ("0123456789.-+eE") && text.containsAnyOf ("0123456789"))
                each.setAttribute (each.getAttributeName (i), juce::String (text.getDoubleValue()));

        for (auto* child : each.getChildIterator())
            normalise (*child);
    };

    normalise (*xml);
    return xml;
}

juce::StringArray findElementDifferences (const juce::ValueTree& expected, const juce::ValueTree& actual)
{
    return phazite::test::findDifferences (*toComparable (expected), *toComparable (actual));
}

//==============================================================================
// The lane's keys and chords as the chord lane's code read them from its element before the value came (Chords.cpp, as it
// was at commit 514dba6e), given the key the song gives before any is written.
std::optional<phazite::Key> readKeyAsBefore (const juce::ValueTree& state)
{
    if (! state.hasProperty (schema::keyRoot))
        return std::nullopt;

    return phazite::Key { juce::jlimit (0, 11, (int) state[schema::keyRoot]),
                          phazite::findScale (state[schema::keyScale].toString()).value_or (phazite::Scale::major) };
}

phazite::BeatPosition getStartAsBefore (const juce::ValueTree& state)
{
    return beat (std::max (0.0, (double) state[schema::chordStart]));
}

std::vector<phazite::LaneKey> getLaneKeysAsBefore (const juce::ValueTree& lane, const phazite::Key& defaultKey)
{
    std::vector<phazite::LaneKey> keys;

    for (const auto& state : lane)
        if (state.hasType (schema::CHORDKEY))
            if (const auto key = readKeyAsBefore (state))
                keys.push_back ({ getStartAsBefore (state), *key });

    std::stable_sort (keys.begin(), keys.end(), [] (const phazite::LaneKey& a, const phazite::LaneKey& b) { return a.start < b.start; });

    if (keys.empty() || keys.front().start > beat (1.0e-6))
        keys.insert (keys.begin(), { phazite::BeatPosition(), defaultKey });

    return keys;
}

std::vector<phazite::PlacedChord> getLaneChordsAsBefore (const juce::ValueTree& lane, const phazite::Key& defaultKey)
{
    const auto keys = getLaneKeysAsBefore (lane, defaultKey);
    std::vector<phazite::PlacedChord> chords;

    for (const auto& state : lane)
    {
        if (! state.hasType (schema::CHORD))
            continue;

        const phazite::BeatRange range { getStartAsBefore (state),
                                         phazite::BeatDuration::fromBeats (std::max (0.0, (double) state[schema::chordLength])) };
        const auto type = phazite::findChordType (state[schema::chordType].toString());

        if (range.isEmpty() || ! type.has_value())
            continue;

        phazite::Numeral numeral;
        numeral.degree = juce::jlimit (1, 7, (int) state.getProperty (schema::chordDegree, 1));
        numeral.accidental = juce::jlimit (-1, 1, (int) state[schema::chordAccidental]);
        numeral.type = *type;
        numeral.inversion = juce::jlimit (0, (int) phazite::getChordIntervals (*type).size() - 1, (int) state[schema::chordInversion]);

        auto key = keys.front().key;

        for (const auto& change : keys)
            if (change.start <= range.getStart() + phazite::BeatDuration::fromBeats (1.0e-6))
                key = change.key;

        chords.push_back ({ range, numeral, key });
    }

    std::stable_sort (chords.begin(), chords.end(), [] (const phazite::PlacedChord& a, const phazite::PlacedChord& b)
                      { return a.range.getStart() < b.range.getStart(); });
    return chords;
}

// The lane's readers on a song against those read from its element as before: its keys, its chords, the key along it and
// where its chords start.
void checkReadersAsBefore (const phazite::Song& song, const juce::ValueTree& lane, const phazite::Key& defaultKey)
{
    const auto keys = phazite::getLaneKeys (song);
    const auto keysBefore = getLaneKeysAsBefore (lane, defaultKey);
    REQUIRE (keys.size() == keysBefore.size());

    for (size_t i = 0; i < keys.size(); ++i)
    {
        CHECK (keys[i].start == keysBefore[i].start);
        CHECK (keys[i].key == keysBefore[i].key);
    }

    const auto chords = phazite::getLaneChords (song);
    const auto chordsBefore = getLaneChordsAsBefore (lane, defaultKey);
    REQUIRE (chords.size() == chordsBefore.size());

    for (size_t i = 0; i < chords.size(); ++i)
    {
        INFO ("chord " << i);
        CHECK (chords[i].range == chordsBefore[i].range);
        CHECK (chords[i].numeral == chordsBefore[i].numeral);
        CHECK (chords[i].key == chordsBefore[i].key);
    }

    for (const auto& state : lane)
    {
        if (state.hasType (schema::CHORD))
        {
            CHECK (phazite::hasLaneChordAt (song, getStartAsBefore (state)));
            CHECK (phazite::hasLaneChordAt (song, getStartAsBefore (state) + phazite::BeatDuration::fromBeats (5.0e-5)));
        }
    }

    CHECK_FALSE (phazite::hasLaneChordAt (song, beat (1000.5)));
}
} // namespace

TEST_CASE ("Chord lane value: the golden project's lane, read into a value and written into an emptied element, is the lane it "
           "was")
{
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto* element = xml->getChildByName ("CHORDLANE");
    REQUIRE (element != nullptr);
    const auto lane = juce::ValueTree::fromXml (*element);

    const auto value = phazite::readHeldChordLane (lane);
    REQUIRE (value.chords.size() == 6);
    REQUIRE (value.keys.size() == 2);
    CHECK (value.chords[4] == ChordEntry { beat (20.0), phazite::BeatDuration::fromBeats (4.0), 2, -1, 2, "majorSeventh" });
    CHECK (value.keys[1] == KeyEntry { beat (24.0), 5, "major" });

    const auto written = writeLane (value);
    const auto differences = findElementDifferences (lane, written);
    CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
    CHECK (phazite::readHeldChordLane (written) == value);
}

TEST_CASE ("Chord lane value: the readers on a song opened with a damaged lane read it as before")
{
    // A chord without a start, a length or a degree; one before the song's start; one of a type Phazite doesn't know; numbers
    // beyond their ranges; a key change without a root, one with a root past B, one before the song's start; a child of another
    // type and a property Phazite doesn't know.
    const auto lane = parse (R"(<CHORDLANE>)"
                             R"(<CHORD type="major"/>)"
                             R"(<CHORD start="-2" length="6" degree="9" accidental="3" inversion="9" type="minorSeventh"/>)"
                             R"(<CHORD start="8" length="4" degree="0" accidental="-4" inversion="-1" type="nothingKnown" colour="red"/>)"
                             R"(<CHORDKEY start="16" keyScale="dorian"/>)"
                             R"(<OTHER start="4"/>)"
                             R"(<CHORD start="12" length="-3" degree="3" type="minor"/>)"
                             R"(<CHORDKEY start="8" keyRoot="14" keyScale="nothingKnown"/>)"
                             R"(<CHORDKEY start="-1" keyRoot="3" keyScale="minor"/>)"
                             R"(<CHORD start="16" length="4" degree="5" type="dominantSeventh"/>)"
                             R"(</CHORDLANE>)");

    // The readers on a song opened with the lane (a file from others, which the loader makes consistent and the song takes
    // into its values: ChordLaneValue.h, readHeldChordLane()), the same as the lane's code read the lane the loader left.
    auto state = phazite::writeSongTree (phazite::makeNewSong());
    state.appendChild (lane.createCopy(), nullptr);
    const auto song = phazite::makeSong (state);
    REQUIRE (song != nullptr);
    REQUIRE (phazite::hasChordLane (*song));
    const auto held = phazite::test::copyWholeSong (*song).getChildWithName (schema::CHORDLANE);
    checkReadersAsBefore (*song, held, phazite::Key {});
    CHECK (phazite::getChordLaneValue (*song) == phazite::readHeldChordLane (held));
    CHECK_FALSE (phazite::getChordLaneValue (*song).chords.empty());
    CHECK_FALSE (phazite::getChordLaneValue (*song).keys.empty());
}

TEST_CASE ("Chord lane value: a lane read as the song holds one reads each property by its description")
{
    // An absent start, length, degree, accidental and inversion as their fallbacks; numbers within their ranges and whole; a
    // key's root that is no number none, one past B kept at B; text read as the song reads a number (a subnormal start, the
    // InconsistentProjectTests' case). The chords apart (keepChordsApart(): the third starts where the first, 4 beats long by
    // its fallback, ends; the second has no length of its own, so it overlaps nothing).
    const auto lane = parse (R"(<CHORDLANE>)"
                             R"(<CHORD type="major"/>)"
                             R"(<CHORD start="-2" length="0" degree="9.6" accidental="3" inversion="20" type="minorSeventh"/>)"
                             R"(<CHORD start=" 4 " length=" 2 " degree="x" accidental="-0.6" type="minor"/>)"
                             R"(<CHORDKEY start="16" keyRoot="none" keyScale="dorian"/>)"
                             R"(<CHORDKEY start="8" keyRoot="14" keyScale="major"/>)"
                             R"(<OTHER start="4"/>)"
                             R"(</CHORDLANE>)");

    const auto value = phazite::readHeldChordLane (lane);
    REQUIRE (value.chords.size() == 3);
    REQUIRE (value.keys.size() == 2);
    CHECK (value.chords[0] == ChordEntry { beat (0.0), phazite::BeatDuration::fromBeats (4.0), 1, 0, 0, "major" });
    CHECK (value.chords[1] == ChordEntry { beat (0.0), phazite::BeatDuration::fromBeats (schema::moreThanZero), 7, 1, 16, "minorSeventh" });
    CHECK (value.chords[2] == ChordEntry { beat (4.0), phazite::BeatDuration::fromBeats (2.0), 1, -1, 0, "minor" });
    CHECK (value.keys[0] == KeyEntry { beat (16.0), -1, "dorian" });
    CHECK (value.keys[1] == KeyEntry { beat (8.0), 11, "major" });
    CHECK (phazite::readHeldChordLane ({}) == ChordLaneValue {});

    // A lane the song wrote reads the same again.
    const auto written = writeLane (value);
    CHECK (phazite::readHeldChordLane (written) == value);
}

TEST_CASE ("Chord lane value: the lane's readers on a lane the model made read it as before, and the lane is there from its first "
           "chord or key")
{
    auto song = phazite::createSong();
    phazite::setLastKey (*song, { 7, phazite::Scale::major });
    CHECK_FALSE (phazite::hasChordLane (*song));
    CHECK (phazite::getChordLaneValue (*song) == ChordLaneValue {});
    checkReadersAsBefore (*song, {}, { 7, phazite::Scale::major });

    {
        const phazite::UndoStep step (*song, "Chords");
        const auto* progression = phazite::findProgression ("canon");
        REQUIRE (progression != nullptr);
        phazite::putProgression (*song, *progression, { beat (8.0), beat (40.0) });
        phazite::setLaneKey (*song, beat (24.0), { 2, phazite::Scale::minor });
        phazite::putLaneChord (*song, { beat (44.0), beat (48.0) }, { 4, -1, phazite::ChordType::majorSeventh, 2 });
    }

    REQUIRE (phazite::hasChordLane (*song));
    const auto lane = phazite::test::copyWholeSong (*song).getChildWithName (schema::CHORDLANE);
    CHECK (phazite::getChordLaneValue (*song) == phazite::readHeldChordLane (lane));
    checkReadersAsBefore (*song, lane, { 7, phazite::Scale::major });
    CHECK (phazite::getLaneKeyAt (*song, beat (30.0)) == phazite::Key { 2, phazite::Scale::minor });

    // Undone, no lane: the keys are the one set last again.
    phazite::undo (*song);
    CHECK_FALSE (phazite::hasChordLane (*song));
    CHECK (phazite::getLaneKeyAt (*song, beat (30.0)) == phazite::Key { 7, phazite::Scale::major });
}

TEST_CASE ("Chord lane value: what enters the song's lane keeps its chords apart, as the lane's writers leave them")
{
    // keepChordsApart(): by their starts, each chord reaching no further than the next one's start, and gone where nothing of
    // it would be left (the later of two at one start stays); a chord without a length of its own stays as it is, no chord's
    // next; the chords that stay in the lane's order, the keys as they are; the same value where nothing overlaps.
    const auto chord = [] (double start, double length, const char* type)
    { return ChordEntry { beat (start), phazite::BeatDuration::fromBeats (length), 1, 0, 0, type }; };

    const ChordLaneValue lane { { chord (8.0, 8.0, "a"), chord (0.0, 16.0, "b"), chord (4.0, 0.0, "c"), chord (8.0, 2.0, "d") },
                                { KeyEntry { beat (0.0), 2, "minor" } } };
    const ChordLaneValue apart { { chord (0.0, 8.0, "b"), chord (4.0, 0.0, "c"), chord (8.0, 2.0, "d") }, lane.keys };
    CHECK_FALSE (phazite::holdsChordsApart (lane));
    CHECK (phazite::keepChordsApart (lane) == apart);
    CHECK (phazite::holdsChordsApart (apart));
    CHECK (phazite::keepChordsApart (apart).isSameAs (apart)); // nothing overlaps: the same value

    // Chords that touch, or overlap by no more than the lane's nearness, are apart.
    const ChordLaneValue touching { { chord (0.0, 4.0 + phazite::chordLaneNear / 2.0, "a"), chord (4.0, 4.0, "b") }, {} };
    CHECK (phazite::holdsChordsApart (touching));
}
