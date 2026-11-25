// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo map as a value (model/base/internal/TempoMapValue.h): the
// conversion between the value and the TEMPOMAP element (the one place that reads and writes TEMPO and TIMESIGNATURE by
// name), round trip through every tempo map of the golden files and through damaged ones; a merge's clash on an entry's
// property; the engine's tempo map made from the value, the same as the one made from the tree before the value came; and
// the time signatures a test gives a song (model/transport/internal/Tempo.h: setTimeSignatureChanges()).
//   phazite_tests.exe --test-case="Tempo map value*"
#include "AudioThreadMonitor.h"
#include "GoldenFiles.h"
#include "PerformanceCounts.h"
#include "TestHelpers.h"
#include "UndoCheck.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/project/internal/Project.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::TempoMapValue;

juce::ValueTree parseMap (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

// A map as XML, each attribute that is a number written as the number (a file's "16" and the value's "16.0" alike), for
// findDifferences() (GoldenFiles.h), which compares text.
std::unique_ptr<juce::XmlElement> toComparable (const juce::ValueTree& map)
{
    auto xml = map.createXml();
    REQUIRE (xml != nullptr);

    const std::function<void (juce::XmlElement&)> normalise = [&normalise] (juce::XmlElement& element)
    {
        for (int i = 0; i < element.getNumAttributes(); ++i)
            if (const auto text = element.getAttributeValue (i);
                text.isNotEmpty() && text.containsOnly ("0123456789.-+eE") && text.containsAnyOf ("0123456789"))
                element.setAttribute (element.getAttributeName (i), juce::String (text.getDoubleValue()));

        for (auto* child : element.getChildIterator())
            normalise (*child);
    };

    normalise (*xml);
    return xml;
}

juce::StringArray findMapDifferences (const juce::ValueTree& expected, const juce::ValueTree& actual)
{
    return phazite::test::findDifferences (*toComparable (expected), *toComparable (actual));
}

// A map written from its value, into an empty TEMPOMAP.
juce::ValueTree writeMap (const TempoMapValue& value)
{
    juce::ValueTree written (schema::TEMPOMAP);
    phazite::writeTempoMap (value, written);
    return written;
}

juce::StringArray getChildTypes (const juce::ValueTree& map)
{
    juce::StringArray types;

    for (const auto& child : map)
        types.add (child.getType().toString() + "@" + juce::String ((double) child[schema::mapBeat]));

    return types;
}

// The engine's tempo map as the tempo map's code made it from the song's tree before the value came (Tempo.cpp, as it was at
// commit bff3a0dc): each kind's elements sorted stably by beat, each property read as the elements hold it, a first at the
// song's start where a kind had none.
phazite::engine::TempoMap makeMapAsBefore (const juce::ValueTree& map, double sampleRate)
{
    const auto getEntries = [&map] (const juce::Identifier& type)
    {
        std::vector<juce::ValueTree> entries;

        for (const auto& entry : map)
            if (entry.hasType (type))
                entries.push_back (entry);

        std::stable_sort (entries.begin(), entries.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                          { return (double) a[schema::mapBeat] < (double) b[schema::mapBeat]; });
        return entries;
    };

    std::vector<phazite::engine::TempoMap::Tempo> tempos;
    std::vector<phazite::engine::TempoMap::TimeSignature> signatures;

    for (const auto& entry : getEntries (schema::TEMPO))
        tempos.push_back ({ phazite::BeatPosition::fromBeats (entry[schema::mapBeat]).inBeats(),
                            (double) entry.getProperty (schema::bpm, 120.0),
                            juce::jlimit (-1.0f, 1.0f, (float) entry.getProperty (schema::tempoCurve, phazite::tempoJump)) });

    for (const auto& entry : getEntries (schema::TIMESIGNATURE))
        signatures.push_back ({ phazite::BeatPosition::fromBeats (entry[schema::mapBeat]).inBeats(),
                                (int) entry.getProperty (schema::numerator, 4), (int) entry.getProperty (schema::denominator, 4) });

    if (tempos.empty())
        tempos.push_back ({ 0.0, 120.0, phazite::tempoJump });

    if (signatures.empty())
        signatures.push_back ({ 0.0, 4, 4 });

    return phazite::engine::TempoMap (std::move (tempos), std::move (signatures), sampleRate);
}

// The same number, both no number alike.
bool isSame (double a, double b)
{
    return (std::isnan (a) && std::isnan (b)) || juce::exactlyEqual (a, b);
}

// Two engine maps alike at a spread of beats, and at every entry's beat and just after it: their seconds, tempos, bars and
// time signatures.
void checkSameEngineMap (const phazite::engine::TempoMap& expected, const phazite::engine::TempoMap& actual, const juce::ValueTree& map)
{
    std::vector<double> beats;

    for (int i = -4; i <= 160; ++i)
        beats.push_back (i * 0.75);

    for (const auto& child : map)
        for (const auto offset : { 0.0, 1.0e-3, 0.5 })
            beats.push_back ((double) child[schema::mapBeat] + offset);

    for (const auto beat : beats)
    {
        CAPTURE (beat);
        CHECK (isSame (actual.toSamples (beat), expected.toSamples (beat)));
        CHECK (isSame (actual.getTempoAt (beat), expected.getTempoAt (beat)));
        CHECK (isSame (actual.getBarsAt (beat), expected.getBarsAt (beat)));
        CHECK (actual.getTimeSignatureAt (beat).numerator == expected.getTimeSignatureAt (beat).numerator);
        CHECK (actual.getTimeSignatureAt (beat).denominator == expected.getTimeSignatureAt (beat).denominator);
    }
}

// The tempo maps of the golden files, as each file holds it: each project's, the command script's and the versions' test
// vector's song.
std::vector<std::pair<juce::String, juce::ValueTree>> readGoldenMaps()
{
    std::vector<std::pair<juce::String, juce::ValueTree>> maps;

    for (const auto* name : { "projects/empty.xml", "projects/representative.xml", "commands/script.xml" })
    {
        const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile (name));
        REQUIRE (xml != nullptr);
        const auto* map = xml->getChildByName ("TEMPOMAP");
        REQUIRE (map != nullptr);
        maps.emplace_back (name, juce::ValueTree::fromXml (*map));
    }

    const auto text = phazite::test::getGoldenFolder().getChildFile ("format/song.txt").loadFileAsString();
    const auto from = text.indexOf ("<TEMPOMAP>"), to = text.indexOf ("</TEMPOMAP>");
    REQUIRE ((from >= 0 && to > from));
    maps.emplace_back ("format/song.txt", parseMap (text.substring (from, to + (int) juce::String ("</TEMPOMAP>").length())));
    return maps;
}

// A step of a merge's path that names an entry of a map, as the merge writes it (SongMerge.cpp: its type and its key).
juce::String stepOf (const juce::ValueTree& entry)
{
    return entry.getType().toString() + ":" + schema::getElementKey (entry);
}

// What a merge's clash chosen did to the map's element before the value: the property set on the first entry its step
// names (SongMerge.cpp: findByPath()), or taken away for a void value; then the map read again.
TempoMapValue setInElement (const TempoMapValue& value, const juce::String& step, const juce::Identifier& property, const juce::var& given)
{
    auto map = writeMap (value);

    for (auto child : map)
    {
        if (stepOf (child) == step)
        {
            if (given.isVoid())
                child.removeProperty (property, nullptr);
            else
                child.setProperty (property, given, nullptr);

            break;
        }
    }

    return phazite::readTempoMapValue (map);
}
} // namespace

//==============================================================================
TEST_CASE ("Tempo map value: every tempo map of the golden files, read into a value and written into an emptied map, is the map "
           "it was, and the engine plays it as before")
{
    for (const auto& [name, map] : readGoldenMaps())
    {
        INFO (juce::String (name));
        const auto value = phazite::readTempoMapValue (map);
        const auto written = writeMap (value);

        // The same tempos and time signatures, with the same properties, in the same order, the file's (tempos, then time
        // signatures, each by beat).
        const auto differences = findMapDifferences (map, written);
        CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
        CHECK (getChildTypes (written) == getChildTypes (map));
        CHECK (phazite::readTempoMapValue (written) == value);
        CHECK (phazite::readTempoEntries (map) == value.tempos);
        CHECK (phazite::readSignatureEntries (map) == value.signatures);

        for (const auto sampleRate : { 1.0, 48000.0 })
            checkSameEngineMap (makeMapAsBefore (map, sampleRate), phazite::getTempoMap (value, sampleRate), map);
    }
}

TEST_CASE ("Tempo map value: the golden projects opened hold their tempo maps whole, and the engine plays each as before")
{
    phazite::test::TemporaryFolder temporary;

    for (const auto* name : { "projects/empty.xml", "projects/representative.xml", "commands/script.xml" })
    {
        INFO (juce::String (name));
        const auto file = temporary.folder.getChildFile (
            juce::String (name).fromLastOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (".", false, false) + ".phazite");
        REQUIRE (phazite::test::getGoldenFolder().getChildFile (name).copyFileTo (file));
        juce::String error;
        const auto project = phazite::Project::open (file, false, error);
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());
        auto& song = project->getSong();
        const auto map = phazite::test::copyWholeSong (song).getChildWithName (schema::TEMPOMAP);

        CHECK (phazite::getTempoMapValue (song) == phazite::readTempoMapValue (map));
        checkSameEngineMap (makeMapAsBefore (map, 1.0), phazite::getTempoMap (song), map);
        checkSameEngineMap (makeMapAsBefore (map, 44100.0), phazite::getTempoMap (song, 44100.0), map);

        const auto changes = phazite::getTempoChanges (song);
        REQUIRE (changes.size() == phazite::getTempoMapValue (song).tempos.size());

        const auto tempos = phazite::getTempoMapValue (song).tempos;

        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto& entry = tempos[i];
            CHECK (changes[i].beat == entry.beat);
            CHECK (juce::exactlyEqual (changes[i].bpm, entry.bpm));
            CHECK (juce::exactlyEqual (changes[i].curve, entry.curve));
        }
    }
}

TEST_CASE ("Tempo map value: reading the song's tempo map through the value allocates no more than reading its tree did")
{
    if (! phazite::test::AudioThreadMonitor::isAvailable())
    {
        MESSAGE ("Allocations aren't counted in this build (AudioThreadMonitor.h): nothing checked here.");
        return;
    }

    // What every clip and plan asks (Tempo.h: getTempoMap(), getTempoChanges(), getTimeSignatureChanges()), with the song
    // holding the map as a value, against its whole state's map read as the tree was read before the value
    // (makeMapAsBefore(), which makes less than the code before did: no TempoChange between the tree and the engine's map).
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    phazite::addTempoChange (song, phazite::BeatPosition::fromBeats (8.0), 140.0);
    const auto map = phazite::test::copyWholeSong (song).getChildWithName (schema::TEMPOMAP);

    const auto count = [] (const std::function<void()>& read) { return phazite::test::countAllocations (read).value_or (0); };
    // The song makes its engine's map once for each change of its tempo map, and keeps it (SongValues.h: PlayedTempoMap): the
    // first call after the change makes it (as a map is made from the value: fromValue) and copies it, those after it copy it.
    const auto before = count ([&map] { juce::ignoreUnused (makeMapAsBefore (map, 1.0)); });
    const auto fromValue = count ([&song] { juce::ignoreUnused (phazite::getTempoMap (phazite::getTempoMapValue (song), 1.0)); });
    const auto made = count ([&song] { juce::ignoreUnused (phazite::getTempoMap (song)); });
    const auto now = count ([&song] { juce::ignoreUnused (phazite::getTempoMap (song)); });
    const auto changesBefore = count (
        [&map]
        {
            std::vector<juce::ValueTree> entries;

            for (const auto& entry : map)
                if (entry.hasType (schema::TEMPO))
                    entries.push_back (entry);

            std::vector<phazite::TempoChange> changes;

            for (const auto& entry : entries)
                changes.push_back ({ phazite::BeatPosition::fromBeats (entry[schema::mapBeat]), (double) entry[schema::bpm] });
        });
    const auto changesNow = count ([&song] { juce::ignoreUnused (phazite::getTempoChanges (song)); });

    MESSAGE ("Allocations, the engine's map: " << before << " from the tree, " << fromValue << " from the value, " << made
                                               << " made and kept once for the change (and copied), " << now
                                               << " copied from the one kept; the tempos: " << changesBefore << " from the tree, "
                                               << changesNow << " through the value");

    // The map made from the value costs no more than from the tree; a read of the one kept, no more again.
    CHECK (fromValue <= before);
    CHECK (now <= fromValue);
    CHECK (changesNow <= changesBefore);
}

TEST_CASE ("Tempo map value: damaged maps read as the tempo map's code read them, and written as it writes them, the differences "
           "listed")
{
    struct Damaged
    {
        const char* about;
        const char* xml;
        bool played = true; // what the engine's map is compared on: not values the loader never lets reach it
    };

    // The first as InconsistentProjectTests.cpp damages the map of the project it opens ("A tempo of zero opens at the slowest
    // tempo..."): a tempo of 0, a time signature of 0/3 and a tempo that is no number appended after it, which the loader
    // brings within the description's ranges before anything plays them (makeConsistent()); the others as another tool's file
    // or a hand's might hold one.
    const Damaged damaged[] = {
        { "a tempo of 0, a time signature of 0/3, a tempo of inf after it",
          R"(<TEMPOMAP><TEMPO beat="0" bpm="0"/><TIMESIGNATURE beat="0" numerator="0" denominator="3"/><TEMPO beat="8" bpm="inf"/></TEMPOMAP>)",
          false },
        { "out of order, two at one beat",
          R"(<TEMPOMAP><TEMPO beat="16" bpm="140"/><TIMESIGNATURE beat="8" numerator="3" denominator="4"/><TEMPO beat="0" bpm="120" curve="0.5"/><TEMPO beat="16" bpm="150"/><TIMESIGNATURE beat="0" numerator="4" denominator="4"/></TEMPOMAP>)" },
        { "a curve of 1 written, and one beyond 1",
          R"(<TEMPOMAP><TEMPO beat="0" bpm="120" curve="1"/><TEMPO beat="4" bpm="100" curve="3"/><TIMESIGNATURE beat="0" numerator="4" denominator="4"/></TEMPOMAP>)" },
        { "empty", R"(<TEMPOMAP/>)" },
        { "properties missing", R"(<TEMPOMAP><TEMPO/><TEMPO beat="4"/><TIMESIGNATURE beat="0"/></TEMPOMAP>)" },
        { "a property and a child Phazite doesn't know",
          R"(<TEMPOMAP colour="red"><TEMPO beat="0" bpm="120" swing="0.2"/><TIMESIGNATURE beat="0" numerator="4" denominator="4"/><LINE/></TEMPOMAP>)" },
        { "a beat before the song",
          R"(<TEMPOMAP><TEMPO beat="-4" bpm="90"/><TEMPO beat="0" bpm="120"/><TIMESIGNATURE beat="0" numerator="4" denominator="4"/></TEMPOMAP>)" },
        { "a beat that is no number",
          R"(<TEMPOMAP><TEMPO beat="0" bpm="120"/><TEMPO beat="nan" bpm="100"/><TIMESIGNATURE beat="0" numerator="4" denominator="4"/></TEMPOMAP>)",
          false },
    };

    for (const auto& each : damaged)
    {
        INFO (juce::String (each.about));
        const auto map = parseMap (each.xml);
        const auto value = phazite::readTempoMapValue (map);
        const auto written = writeMap (value);

        // Written again from what it reads, the same text (a number that is no number too): what is written is the value whole.
        CHECK (writeMap (phazite::readTempoMapValue (written)).toXmlString() == written.toXmlString());

        // At least one of each kind, each kind by beat.
        CHECK (! value.tempos.empty());
        CHECK (! value.signatures.empty());
        CHECK (std::is_sorted (value.tempos.begin(), value.tempos.end(), [] (const auto& a, const auto& b) { return a.beat < b.beat; }));
        CHECK (std::is_sorted (value.signatures.begin(), value.signatures.end(),
                               [] (const auto& a, const auto& b) { return a.beat < b.beat; }));

        if (each.played)
            for (const auto sampleRate : { 1.0, 48000.0 })
                checkSameEngineMap (makeMapAsBefore (map, sampleRate), phazite::getTempoMap (value, sampleRate), map);

        // What differs from the map as it was: listed, for the report.
        auto differences = findMapDifferences (map, written);

        if (getChildTypes (written) != getChildTypes (map))
            differences.add ("order: " + getChildTypes (map).joinIntoString (" ") + " -> " + getChildTypes (written).joinIntoString (" "));

        MESSAGE (juce::String (each.about) << ":\n  "
                                           << (differences.isEmpty() ? juce::String ("none") : differences.joinIntoString ("\n  ")));
    }
}

TEST_CASE ("Tempo map value: entries at one beat keep the order they came in, read, written and read again")
{
    const auto map =
        parseMap (R"(<TEMPOMAP><TEMPO beat="8" bpm="100"/><TEMPO beat="0" bpm="120"/><TEMPO beat="8" bpm="110"/>)"
                  R"(<TIMESIGNATURE beat="4" numerator="7" denominator="8"/><TEMPO beat="8" bpm="90"/>)"
                  R"(<TIMESIGNATURE beat="0" numerator="4" denominator="4"/><TIMESIGNATURE beat="4" numerator="5" denominator="4"/>)"
                  R"(</TEMPOMAP>)");
    const auto value = phazite::readTempoMapValue (map);
    std::vector<double> bpms;
    std::vector<int> numerators;

    for (const auto& tempo : value.tempos)
        bpms.push_back (tempo.bpm);

    for (const auto& signature : value.signatures)
        numerators.push_back (signature.numerator);

    CHECK (bpms == std::vector<double> { 120.0, 100.0, 110.0, 90.0 });
    CHECK (numerators == std::vector<int> { 4, 7, 5 });

    const auto again = phazite::readTempoMapValue (writeMap (value));
    CHECK (again == value);
    CHECK (writeMap (again).isEquivalentTo (writeMap (value)));
    CHECK (getChildTypes (writeMap (value))
           == juce::StringArray { "TEMPO@" + juce::String (0.0), "TEMPO@" + juce::String (8.0), "TEMPO@" + juce::String (8.0),
                                  "TEMPO@" + juce::String (8.0), "TIMESIGNATURE@" + juce::String (0.0),
                                  "TIMESIGNATURE@" + juce::String (4.0), "TIMESIGNATURE@" + juce::String (4.0) });

    // A value compared: the same by identity only while shared.
    auto copy = value;
    CHECK (copy.isSameAs (value));
    copy.tempos = copy.tempos.set (1, copy.tempos[1]);
    CHECK_FALSE (copy.isSameAs (value));
    CHECK (copy == value);
    CHECK (phazite::makeDefaultTempoMap() == phazite::readTempoMapValue (juce::ValueTree (schema::TEMPOMAP)));
    CHECK (phazite::makeDefaultTempoMap() == phazite::readTempoMapValue ({}));
}

TEST_CASE ("Tempo map value: a merge's clash sets each property of a tempo and of a time signature as the element held it, "
           "and nothing for a step that names no entry or a property that isn't the entry's")
{
    const auto map =
        parseMap (R"(<TEMPOMAP><TEMPO beat="0" bpm="120" curve="0.25"/><TEMPO beat="16" bpm="140"/><TEMPO beat="16" bpm="150"/>)"
                  R"(<TEMPO beat="32" bpm="90"/><TIMESIGNATURE beat="0" numerator="4" denominator="4"/>)"
                  R"(<TIMESIGNATURE beat="24" numerator="6" denominator="8"/></TEMPOMAP>)");
    const auto value = phazite::readTempoMapValue (map);

    struct Clash
    {
        const char* step;
        const juce::Identifier& property;
        juce::var given;
    };

    // Each property of each kind given a value, a void one (as if the element hadn't it) and one beyond its range: the value
    // set as the map's element would hold it, read again.
    const Clash clashes[] = {
        { "TEMPO:0.000000", schema::bpm, 128.5 },
        { "TEMPO:0.000000", schema::bpm, {} },
        { "TEMPO:16.000000", schema::bpm, "133" }, // the first at 16
        { "TEMPO:0.000000", schema::tempoCurve, 0.0f },
        { "TEMPO:0.000000", schema::tempoCurve, {} },
        { "TEMPO:16.000000", schema::tempoCurve, -0.5f },
        { "TEMPO:16.000000", schema::tempoCurve, 4.0 },
        { "TEMPO:32.000000", schema::mapBeat, 8.0 },
        { "TEMPO:32.000000", schema::mapBeat, 64.0 },
        { "TEMPO:0.000000", schema::mapBeat, 2.0 },
        { "TEMPO:0.000000", schema::mapBeat, 16.0 },  // later, onto two others' beat: before them
        { "TEMPO:32.000000", schema::mapBeat, 16.0 }, // earlier, onto it: after them
        { "TEMPO:16.000000", schema::mapBeat, 16.0 }, // its own beat: where it was
        { "TIMESIGNATURE:24.000000", schema::numerator, 7 },
        { "TIMESIGNATURE:24.000000", schema::numerator, {} },
        { "TIMESIGNATURE:24.000000", schema::denominator, 16 },
        { "TIMESIGNATURE:0.000000", schema::denominator, {} },
        { "TIMESIGNATURE:24.000000", schema::mapBeat, 12.0 },
        { "TIMESIGNATURE:24.000000", schema::mapBeat, {} }, // to the start, after the one there
    };

    for (const auto& clash : clashes)
    {
        INFO (juce::String (clash.step) << "@" << clash.property.toString() << " = " << clash.given.toString());
        auto set = value;
        REQUIRE (phazite::setTempoMapProperty (set, clash.step, clash.property, clash.given));
        CHECK (set == setInElement (value, clash.step, clash.property, clash.given));
    }

    // A beat set on one of two at one beat moves the first the step names; one moved earlier onto another's beat goes after it.
    {
        auto set = value;
        REQUIRE (phazite::setTempoMapProperty (set, "TEMPO:16.000000", schema::mapBeat, 20.0));
        REQUIRE (set.tempos.size() == 4);
        CHECK (juce::exactlyEqual (set.tempos[1].bpm, 150.0));
        CHECK (set.tempos[2].beat == phazite::BeatPosition::fromBeats (20.0));
        CHECK (juce::exactlyEqual (set.tempos[2].bpm, 140.0));

        REQUIRE (phazite::setTempoMapProperty (set, "TEMPO:32.000000", schema::mapBeat, 16.0));
        CHECK (set.tempos[1].beat == phazite::BeatPosition::fromBeats (16.0));
        CHECK (juce::exactlyEqual (set.tempos[1].bpm, 150.0));
        CHECK (set.tempos[2].beat == phazite::BeatPosition::fromBeats (16.0));
        CHECK (juce::exactlyEqual (set.tempos[2].bpm, 90.0));
    }

    // Nothing for a step that names no entry, or another kind; for a property that isn't the entry's kind's.
    const Clash refused[] = {
        { "TEMPO:3.000000", schema::bpm, 100.0 },
        { "TEMPO:16", schema::bpm, 100.0 },
        { "TIMESIGNATURE:16.000000", schema::numerator, 3 },
        { "CHORD:0.000000", schema::bpm, 100.0 },
        { "TEMPOMAP:TEMPOMAP", schema::bpm, 100.0 },
        { "", schema::bpm, 100.0 },
        { "TEMPO:0.000000", schema::numerator, 3 },
        { "TEMPO:0.000000", schema::denominator, 8 },
        { "TIMESIGNATURE:0.000000", schema::bpm, 100.0 },
        { "TIMESIGNATURE:0.000000", schema::tempoCurve, 0.0 },
        { "TEMPO:0.000000", schema::trackName, "Lead" },
    };

    for (const auto& clash : refused)
    {
        INFO (juce::String (clash.step) << "@" << clash.property.toString());
        auto set = value;
        CHECK_FALSE (phazite::setTempoMapProperty (set, clash.step, clash.property, clash.given));
        CHECK (set.isSameAs (value));
    }
}

TEST_CASE ("Tempo map value: time signatures given along the song are one exact undo step, read back as given, and nothing for "
           "what the song holds already or can't take")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    const auto signaturesOf = [&song]
    {
        std::vector<juce::String> held;

        for (const auto& change : phazite::getTimeSignatureChanges (song))
            held.push_back (juce::String (juce::roundToInt (change.beat.inBeats())) + ":" + juce::String (change.signature.numerator) + "/"
                            + juce::String (change.signature.denominator));

        return held;
    };

    // Given out of order: put in order by beat.
    undo.check ("Change Time Signatures",
                [&]
                {
                    phazite::setTimeSignatureChanges (song, { { phazite::BeatPosition::fromBeats (6.0), { 4, 4 } },
                                                              { phazite::BeatPosition(), { 6, 8 } },
                                                              { phazite::BeatPosition::fromBeats (14.0), { 7, 8 } } });
                });
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Change Time Signatures"));
    CHECK (signaturesOf() == std::vector<juce::String> { "0:6/8", "6:4/4", "14:7/8" });
    CHECK (phazite::getTimeSignature (song) == phazite::TimeSignature { 6, 8 });
    CHECK (phazite::getBars (song).getBarAt (phazite::BeatPosition::fromBeats (6.0)) == 2);

    undo.check ("Fewer Time Signatures", [&] { phazite::setTimeSignatureChanges (song, { { phazite::BeatPosition(), { 3, 4 } } }); });
    CHECK (signaturesOf() == std::vector<juce::String> { "0:3/4" });

    // Nothing, no step: the same again, none, an invalid one, one before the song, a first after the song's start.
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    const auto description = song.getUndoManager().getUndoDescription();

    for (const auto& changes : std::vector<std::vector<phazite::TimeSignatureChange>> {
             { { phazite::BeatPosition(), { 3, 4 } } },
             {},
             { { phazite::BeatPosition(), { 4, 3 } } },
             { { phazite::BeatPosition(), { 4, 4 } }, { phazite::BeatPosition::fromBeats (-2.0), { 4, 4 } } },
             { { phazite::BeatPosition::fromBeats (2.0), { 4, 4 } } } })
    {
        phazite::setTimeSignatureChanges (song, changes);
        CHECK (signaturesOf() == std::vector<juce::String> { "0:3/4" });
    }

    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
    CHECK (song.getUndoManager().getUndoDescription() == description);
    undo.walkBackAndForth();
}
