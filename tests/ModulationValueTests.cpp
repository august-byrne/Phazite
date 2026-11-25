// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's modulation as a value (model/base/internal/ModulationValue.h): the round trip of its elements through the golden
// files' modulators and macros and through damaged ones; the readers; the value's helpers and its IDs; a merge's clash on a
// modulator's property; and the readers of a song (Modulation.h) against what its elements hold, on a song made through the model
// and on one opened through the loader.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongState.h"
#include "model/devices/internal/Parameters.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::DeviceID;
using phazite::ModulatorID;
using phazite::ModulatorPlace;
using phazite::ModulatorValue;
using phazite::ParameterRef;
using phazite::SongModulation;
using phazite::TrackID;
using phazite::devices::ModulatorKind;

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

// Every MODULATOR and MACRO element under one, in document order.
std::vector<juce::ValueTree> findModulatorElements (const juce::ValueTree& tree)
{
    std::vector<juce::ValueTree> found;
    const std::function<void (const juce::ValueTree&)> visit = [&found, &visit] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
                found.push_back (child);

            visit (child);
        }
    };

    visit (tree);
    return found;
}

// An element as XML for findDifferences() (GoldenFiles.h), which compares text: each attribute that is a number, or numbers
// separated by spaces, written as those numbers (a file's "1" and the value's "1.0" alike); and each element's children sorted,
// stably, by their tags, as the golden files write them (GoldenFiles.h: N4), so that only what Phazite's writers keep in
// order is compared in order (a modulator's MODULATES; not where its VALUES stand among them, which follows its history).
std::unique_ptr<juce::XmlElement> toComparable (const juce::ValueTree& element)
{
    auto xml = element.createXml();
    REQUIRE (xml != nullptr);

    const std::function<void (juce::XmlElement&)> normalise = [&normalise] (juce::XmlElement& each)
    {
        for (int i = 0; i < each.getNumAttributes(); ++i)
        {
            const auto text = each.getAttributeValue (i);
            auto tokens = juce::StringArray::fromTokens (text, " ", {});
            const auto allNumbers =
                ! tokens.isEmpty()
                && std::all_of (
                    tokens.begin(), tokens.end(), [] (const juce::String& token)
                    { return token.isNotEmpty() && token.containsOnly ("0123456789.-+eE") && token.containsAnyOf ("0123456789"); });

            if (allNumbers)
            {
                for (auto& token : tokens)
                    token = juce::String (token.getDoubleValue());

                each.setAttribute (each.getAttributeName (i), tokens.joinIntoString (" "));
            }
        }

        std::vector<juce::XmlElement*> children;

        for (auto* child : each.getChildIterator())
            children.push_back (child);

        std::stable_sort (children.begin(), children.end(),
                          [] (const juce::XmlElement* a, const juce::XmlElement* b) { return a->getTagName() < b->getTagName(); });

        for (auto* child : children)
        {
            each.removeChildElement (child, false);
            normalise (*child);
        }

        for (auto* child : children)
            each.addChildElement (child);
    };

    normalise (*xml);
    return xml;
}

juce::StringArray findElementDifferences (const juce::ValueTree& expected, const juce::ValueTree& actual)
{
    return phazite::test::findDifferences (*toComparable (expected), *toComparable (actual));
}

// Read as the song holds it, written, and read again: the same value, and the same element written twice.
void checkSavedTwiceTheSame (const juce::ValueTree& element)
{
    const auto place = phazite::readModulatorPlace (element);
    const auto held = phazite::readHeldModulator (element, place);

    if (! held.has_value())
        return;

    const auto written = phazite::writeModulatorElement (*held);
    const auto again = phazite::readHeldModulator (written, place);
    REQUIRE (again.has_value());
    CHECK (*again == *held);
    const auto differences = findElementDifferences (written, phazite::writeModulatorElement (*again));
    CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
}

ModulatorValue makeModulator (std::uint64_t id, ModulatorPlace place, std::optional<ModulatorKind> kind = ModulatorKind::lfo)
{
    ModulatorValue value;
    value.id = ModulatorID::fromRaw (id);
    value.place = place;
    value.kind = kind;
    return value;
}

std::vector<ModulatorID> toVector (const immer::flex_vector<ModulatorID>* list)
{
    return list != nullptr ? std::vector<ModulatorID> (list->begin(), list->end()) : std::vector<ModulatorID>();
}

std::vector<ModulatorID> ids (std::initializer_list<std::uint64_t> raw)
{
    std::vector<ModulatorID> made;

    for (const auto each : raw)
        made.push_back (ModulatorID::fromRaw (each));

    return made;
}

int findLfoParameter (const char* id)
{
    return phazite::devices::findModulatorParameter (ModulatorKind::lfo, id);
}

// A song's file of a track with damaged modulators and macros (as tests/InconsistentProjectTests.cpp's): a modulator with a
// property and a child Phazite doesn't know, a rate past its range, a choice by a name it hasn't, a parameter it hasn't, an
// amount past its range, two modulations of one parameter, one of no device and one of no parameter; a step sequencer whose
// steps hold what is no finite number, beyond their range and zeros at the end; a modulator of a type Phazite doesn't know;
// a macro whose value is past its range and whose amount has more digits than Phazite writes; a macro without an ID; and a
// container's macro.
juce::ValueTree makeDamagedSong()
{
    auto state = phazite::writeSongTree (phazite::makeNewSong());
    const auto track =
        parse (R"(<TRACK id="9005" kind="track" name="Lead" fader="9006">)"
               R"(<DEVICE id="9007" type="phaziteSynth"/>)"
               R"(<CONTAINER id="9020" name="Container"><CHAIN id="9021" name="Chain 1"/>)"
               R"(<MACRO id="9022" name="Inside" value="0.5"><MODULATES device="9021" parameter="volume" amount="0.25"/></MACRO>)"
               R"(</CONTAINER>)"
               R"(<MODULATOR id="9010" type="lfo" off="1" steps="0.5" shape="round">)"
               R"(<VALUES rate="1e3" bipolar="Bipolar" wave="Nothing" unknown="3"/>)"
               R"(<MODULATES device="9007" parameter="cutoff" amount="3"/>)"
               R"(<MODULATES device="9007" parameter="cutoff" amount="0.25"/>)"
               R"(<MODULATES device="0" parameter="cutoff" amount="0.5"/>)"
               R"(<MODULATES device="9007" parameter="" amount="0.5"/>)"
               R"(<OTHER/>)"
               R"(</MODULATOR>)"
               R"(<MODULATOR id="9011" type="steps" steps="0.5 1e12345678901 -3 nan 0.25 0 0"/>)"
               R"(<MODULATOR id="9012" type="someoneElses"/>)"
               R"(<MACRO id="9013" name="Air" value="3"><MODULATES device="9007" parameter="cutoff" amount="-0.123456"/></MACRO>)"
               R"(<MACRO name="No ID"/>)"
               R"(</TRACK>)");
    state.getChildWithName (schema::TRACKS).appendChild (track, nullptr);
    return state;
}
} // namespace

TEST_CASE ("Modulation value: the golden files' modulators and macros, read into values and written again, are the elements they "
           "were")
{
    for (const auto* name : { "projects/representative.xml", "commands/script.xml" })
    {
        INFO (juce::String (name));
        const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile (name));
        REQUIRE (xml != nullptr);
        const auto song = juce::ValueTree::fromXml (*xml);
        const auto elements = findModulatorElements (song);

        if (juce::String (name).contains ("representative"))
            CHECK (elements.size() >= 3); // its modulators and macros, and its containers'

        for (const auto& element : elements)
        {
            INFO (element.toXmlString());
            const auto place = phazite::readModulatorPlace (element);
            const auto held = phazite::readHeldModulator (element, place);
            REQUIRE (held.has_value());

            // As Phazite's code read it, the same: what Phazite wrote holds nothing the song's reading would change.
            const auto lenient = phazite::readModulatorValue (element, place);
            REQUIRE (lenient.has_value());
            CHECK (*lenient == *held);

            // Written as Phazite's writers write it: the element it was, property for property; read again, the same.
            const auto written = phazite::writeModulatorElement (*held);
            const auto differences = findElementDifferences (element, written);
            CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
            CHECK (phazite::readHeldModulator (written, place) == held);
            checkSavedTwiceTheSame (element);
        }

        // The song's modulation, every one of them by its ID, each owner's in its order.
        const auto modulation = phazite::readHeldModulation (song);
        CHECK (modulation.modulators.size() == elements.size());
        CHECK (phazite::holdsItsLists (modulation));

        for (const auto& element : elements)
        {
            const auto* found = modulation.find (ModulatorID::fromRaw (phazite::readID (element[schema::modulatorID])));
            REQUIRE (found != nullptr);
            CHECK (*found == *phazite::readHeldModulator (element, phazite::readModulatorPlace (element)));
        }
    }
}

TEST_CASE ("Modulation value: damaged modulators read as Phazite's code read them, and as the song holds them, written so twice "
           "the same")
{
    const auto state = makeDamagedSong();
    const auto lead = state.getChildWithName (schema::TRACKS).getChildWithProperty (schema::trackID, 9005);
    REQUIRE (lead.isValid());
    const auto element = [&lead] (int id) { return lead.getChildWithProperty (schema::modulatorID, id); };
    const ModulatorPlace onLead { .track = TrackID::fromRaw (9005) };

    // As Phazite's code read it: every value of a parameter it has as it was (a rate past its range too, a choice it hasn't
    // as 0), every modulation as it was, the steps as many as there are, each within -1 to 1 and 0 for no finite number.
    const auto lfo = phazite::readModulatorValue (element (9010), onLead);
    REQUIRE (lfo.has_value());
    CHECK (lfo->kind == ModulatorKind::lfo);
    CHECK (lfo->off);
    CHECK (lfo->place == onLead);
    CHECK (lfo->getParameterValue (findLfoParameter ("rate")) == doctest::Approx (1000.0f));
    CHECK (lfo->getParameterValue (findLfoParameter ("bipolar")) == doctest::Approx (1.0f));
    CHECK (lfo->values.size() == 3); // rate, bipolar and wave, not one it hasn't
    CHECK (lfo->modulations.size() == 4);
    CHECK (lfo->modulations[0].amount == doctest::Approx (3.0f));
    REQUIRE (lfo->steps.size() == 1);

    const auto steps = phazite::readModulatorValue (element (9011), onLead);
    REQUIRE (steps.has_value());
    CHECK (std::vector<float> (steps->steps.begin(), steps->steps.end())
           == std::vector<float> { 0.5f, 0.0f, -1.0f, 0.0f, 0.25f, 0.0f, 0.0f });
    CHECK_FALSE (phazite::readModulatorValue (element (9012), onLead).has_value()); // a type Phazite doesn't know
    CHECK_FALSE (phazite::readHeldModulator (element (9012), onLead).has_value());

    const auto air = phazite::readModulatorValue (element (9013), onLead);
    REQUIRE (air.has_value());
    CHECK (air->isMacro());
    CHECK (air->name == "Air");
    CHECK (air->value == doctest::Approx (3.0f));
    REQUIRE (air->modulations.size() == 1);
    CHECK (air->modulations[0].amount == doctest::Approx (-0.123456f));

    const auto noID = phazite::readModulatorValue (lead.getChildWithProperty (schema::macroName, "No ID"), onLead);
    REQUIRE (noID.has_value());
    CHECK_FALSE (noID->id.isValid());
    CHECK_FALSE (phazite::readHeldModulator (lead.getChildWithProperty (schema::macroName, "No ID"), onLead).has_value());

    // As the song holds it: each value within its parameter's range, none at its default nor of a choice it hasn't; one
    // modulation of each parameter, of a device and a parameter, its amount within -1 to 1 and to a ten-thousandth; the
    // steps without the zeros at the end; a macro's value within 0 to 1.
    const auto heldLfo = phazite::readHeldModulator (element (9010), onLead);
    REQUIRE (heldLfo.has_value());
    CHECK (heldLfo->getParameterValue (findLfoParameter ("rate")) == doctest::Approx (50.0f));
    CHECK (heldLfo->values.size() == 2); // rate and bipolar
    REQUIRE (heldLfo->modulations.size() == 1);
    CHECK (heldLfo->modulations[0].parameter == ParameterRef { DeviceID::fromRaw (9007), "cutoff" });
    CHECK (heldLfo->modulations[0].amount == doctest::Approx (1.0f));

    const auto heldSteps = phazite::readHeldModulator (element (9011), onLead);
    REQUIRE (heldSteps.has_value());
    CHECK (std::vector<float> (heldSteps->steps.begin(), heldSteps->steps.end()) == std::vector<float> { 0.5f, 0.0f, -1.0f, 0.0f, 0.25f });

    const auto heldAir = phazite::readHeldModulator (element (9013), onLead);
    REQUIRE (heldAir.has_value());
    CHECK (heldAir->value == doctest::Approx (1.0f));
    CHECK (heldAir->modulations[0].amount == doctest::Approx (-0.1235f));

    // A container's macro: its place its container's, on its channel.
    const auto inside =
        phazite::readModulatorPlace (lead.getChildWithProperty (schema::deviceID, 9020).getChildWithProperty (schema::modulatorID, 9022));
    CHECK (inside == ModulatorPlace { .track = TrackID::fromRaw (9005), .container = DeviceID::fromRaw (9020) });

    // Written as Phazite writes them (nothing of what it doesn't know), read again the same, and written twice the same.
    const auto written = phazite::writeModulatorElement (*heldLfo);
    CHECK_FALSE (written.hasProperty ("shape"));
    CHECK_FALSE (written.getChildWithName ("OTHER").isValid());
    CHECK (written.getChildWithName (schema::VALUES)["bipolar"].toString() == "Bipolar"); // a choice by its name
    CHECK ((bool) written[schema::deviceOff]);
    CHECK (phazite::writeModulatorElement (*heldSteps)[schema::modulatorSteps].toString()
           == "0.5 " + juce::String (0.0) + " " + juce::String (-1.0) + " " + juce::String (0.0) + " 0.25");

    for (const auto& each : findModulatorElements (state))
        checkSavedTwiceTheSame (each);

    // Opened through the loader, which makes the song consistent: what it keeps of each the song holds as the held reader
    // reads the damaged element, and the song's readers read it so (Modulation.h).
    const auto song = phazite::makeSong (state.createCopy());
    REQUIRE (song != nullptr);
    const auto damagedElements = findModulatorElements (state);
    auto loadedCount = 0;

    const auto whole = phazite::test::copyWholeSong (*song); // kept, as its elements' places are read from their parents

    for (const auto& loaded : findModulatorElements (whole))
    {
        const auto id = ModulatorID::fromRaw (phazite::readID (loaded[schema::modulatorID]));
        INFO (loaded.toXmlString());
        checkSavedTwiceTheSame (loaded);
        REQUIRE (phazite::getModulatorValue (*song, id).has_value());
        const auto held = phazite::readHeldModulator (loaded, phazite::readModulatorPlace (loaded));
        REQUIRE (held.has_value());

        // One given an ID as it opened (the macro without one) has none to be found by among the damaged.
        const auto damaged = std::find_if (damagedElements.begin(), damagedElements.end(), [id] (const juce::ValueTree& each)
                                           { return phazite::readID (each[schema::modulatorID]) == id.getRaw(); });

        if (damaged == damagedElements.end())
            continue;

        const auto fromDamaged = phazite::readHeldModulator (*damaged, phazite::readModulatorPlace (*damaged));
        REQUIRE (fromDamaged.has_value());
        CHECK (*fromDamaged == *held);
        ++loadedCount;
    }

    CHECK (loadedCount == 4); // the LFO, the step sequencer, the macro and the container's; not the type Phazite doesn't know
    CHECK_FALSE (phazite::getModulatorValue (*song, ModulatorID::fromRaw (9012)).has_value()); // gone as it opened
}

TEST_CASE ("Modulation value: the helpers keep each modulator in its owner's list, in its order, and holdsItsLists() says so")
{
    const ModulatorPlace track { .track = TrackID::fromRaw (5) }, other { .track = TrackID::fromRaw (6) };
    const ModulatorPlace master { .onMaster = true }, container { .track = TrackID::fromRaw (5), .container = DeviceID::fromRaw (20) };

    SongModulation modulation;
    modulation = phazite::withModulator (modulation, makeModulator (10, track));
    modulation = phazite::withModulator (modulation, makeModulator (11, track, std::nullopt));
    modulation = phazite::withModulator (modulation, makeModulator (12, track), 0); // first
    modulation = phazite::withModulator (modulation, makeModulator (13, master));
    modulation = phazite::withModulator (modulation, makeModulator (14, container, std::nullopt));
    CHECK (phazite::holdsItsLists (modulation));
    CHECK (toVector (modulation.findList (track)) == ids ({ 12, 10, 11 }));
    CHECK (toVector (modulation.findList (master)) == ids ({ 13 }));
    CHECK (toVector (modulation.findList (container)) == ids ({ 14 }));
    CHECK (modulation.channels.find (0) != nullptr); // the master's, under 0

    // Changed in place, it keeps its place; on another owner, at that owner's end; an owner left without any has no list.
    auto changed = makeModulator (10, track);
    changed.off = true;
    const auto inPlace = phazite::withModulator (modulation, changed);
    CHECK (toVector (inPlace.findList (track)) == ids ({ 12, 10, 11 }));
    CHECK (inPlace.find (ModulatorID::fromRaw (10))->off);
    CHECK_FALSE (inPlace.isSameAs (modulation));

    const auto moved = phazite::withModulator (modulation, makeModulator (13, other));
    CHECK (moved.findList (master) == nullptr);
    CHECK (toVector (moved.findList (other)) == ids ({ 13 }));
    CHECK (phazite::holdsItsLists (moved));

    // A container's macro whose container moved to another channel stays in its container's list.
    const auto containerMoved =
        phazite::withModulator (modulation, makeModulator (14, { .onMaster = true, .container = DeviceID::fromRaw (20) }, std::nullopt));
    CHECK (toVector (containerMoved.findList (container)) == ids ({ 14 }));
    CHECK (containerMoved.find (ModulatorID::fromRaw (14))->place.onMaster);

    // Taken out, and an owner's order.
    const auto without = phazite::withoutModulator (modulation, ModulatorID::fromRaw (12));
    CHECK (toVector (without.findList (track)) == ids ({ 10, 11 }));
    CHECK (without.find (ModulatorID::fromRaw (12)) == nullptr);
    CHECK (phazite::withoutModulator (modulation, ModulatorID::fromRaw (99)).isSameAs (modulation));

    immer::flex_vector<ModulatorID> order { ModulatorID::fromRaw (11), ModulatorID::fromRaw (12), ModulatorID::fromRaw (10) };
    CHECK (toVector (phazite::withOwnerOrder (modulation, track, order).findList (track)) == ids ({ 11, 12, 10 }));
    immer::flex_vector<ModulatorID> others { ModulatorID::fromRaw (11), ModulatorID::fromRaw (13), ModulatorID::fromRaw (10) };
    CHECK (phazite::withOwnerOrder (modulation, track, others).isSameAs (modulation)); // not the same IDs

    // What holdsItsLists() refuses: one listed twice, one in another owner's list, a list without any.
    auto broken = modulation;
    broken.channels = broken.channels.set (6, immer::flex_vector<ModulatorID> { ModulatorID::fromRaw (10) });
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = modulation;
    broken.containers = broken.containers.set (21, immer::flex_vector<ModulatorID>());
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = modulation;
    broken.modulators = broken.modulators.set (15, makeModulator (15, track));
    CHECK_FALSE (phazite::holdsItsLists (broken));

    // Its IDs: the largest, each held, each named.
    auto modulating = makeModulator (16, track);
    modulating.modulations =
        modulating.modulations.push_back ({ .parameter = ParameterRef { DeviceID::fromRaw (30), "rate" }, .amount = 0.5f });
    modulation = phazite::withModulator (modulation, modulating);
    CHECK (phazite::findLargestIDIn (modulation) == 16);
    std::vector<std::uint64_t> held, named;
    phazite::forEachIDHeld (modulation, [&held] (std::uint64_t id) { held.push_back (id); });
    phazite::forEachIDNamed (modulation, [&named] (std::uint64_t id) { named.push_back (id); });
    std::sort (held.begin(), held.end());
    CHECK (held == std::vector<std::uint64_t> { 10, 11, 12, 13, 14, 16 });
    CHECK (named == std::vector<std::uint64_t> { 30 });
    CHECK (phazite::findLargestIDIn (phazite::withModulator ({}, makeModulator (phazite::schema::maxID, track))) == 0);
}

TEST_CASE ("Modulation value: a merge's clash sets each of a modulator's and a macro's own properties as the element held it, "
           "and nothing for one it hasn't")
{
    auto lfo = makeModulator (10, { .track = TrackID::fromRaw (5) });
    lfo.values = lfo.values.set ((std::uint32_t) findLfoParameter ("rate"), 4.0f);
    lfo.values = lfo.values.set ((std::uint32_t) findLfoParameter ("bipolar"), 1.0f);

    CHECK (phazite::setModulatorProperty (lfo, schema::deviceOff, true));
    CHECK (lfo.off);
    CHECK (phazite::setModulatorProperty (lfo, schema::deviceOff, {}));
    CHECK_FALSE (lfo.off);
    CHECK (phazite::setModulatorProperty (lfo, schema::modulatorSteps, "0.5 -2 0"));
    CHECK (std::vector<float> (lfo.steps.begin(), lfo.steps.end()) == std::vector<float> { 0.5f, -1.0f });
    CHECK (phazite::setModulatorProperty (lfo, schema::modulatorID, 12));
    CHECK (lfo.id == ModulatorID::fromRaw (12));
    CHECK_FALSE (phazite::setModulatorProperty (lfo, schema::macroName, "Air")); // a macro's
    CHECK_FALSE (phazite::setModulatorProperty (lfo, schema::modulatorType, "someoneElses"));
    CHECK (lfo.kind == ModulatorKind::lfo);

    // Its type: another kind keeps the values of the parameters it has by their names (a random source's rate and polarity).
    CHECK (phazite::setModulatorProperty (lfo, schema::modulatorType, "random"));
    CHECK (lfo.kind == ModulatorKind::random);
    const auto randomRate = phazite::devices::findModulatorParameter (ModulatorKind::random, "rate");
    CHECK (lfo.getParameterValue (randomRate) == doctest::Approx (4.0f));
    CHECK (lfo.values.size() == 2);

    auto macro = makeModulator (11, { .track = TrackID::fromRaw (5) }, std::nullopt);
    CHECK (phazite::setModulatorProperty (macro, schema::macroName, "Air"));
    CHECK (macro.name == "Air");
    CHECK (phazite::setModulatorProperty (macro, schema::macroValue, 2.0));
    CHECK (macro.value == doctest::Approx (1.0f));
    CHECK (phazite::setModulatorProperty (macro, schema::macroValue, {}));
    CHECK (macro.value == doctest::Approx (0.0f));
    CHECK_FALSE (phazite::setModulatorProperty (macro, schema::modulatorType, "lfo"));
    CHECK_FALSE (phazite::setModulatorProperty (macro, "colour", "red"));
}

TEST_CASE ("Modulation value: the song's readers read its modulators and macros as its elements hold them, those made through the "
           "model")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Keys");
    const auto synth = s.setInstrument (track, "phaziteSynth");
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    s.setModulatorParameter (lfo, "rate", 3.0f);
    s.modulate (lfo, { synth, "cutoff" }, 0.25f);
    const auto steps = s.addModulator (track, phazite::ModulatorType::steps);
    const auto macro = s.addModulator (track, phazite::ModulatorType::macro);
    s.setMacroValue (macro, 0.5f);
    s.modulate (macro, s.getVolumeParameter (track), -0.5f);
    const auto master = s.addModulator (phazite::getMasterID (song), phazite::ModulatorType::random);

    // Each as the song's whole state writes it, read: the song's value of it, at its place.
    const auto whole = phazite::test::copyWholeSong (song);
    const auto modulation = phazite::getModulationValue (song);
    CHECK (phazite::holdsItsLists (modulation));
    CHECK (modulation.modulators.size() == 4);

    for (const auto& element : findModulatorElements (whole))
    {
        const auto id = ModulatorID::fromRaw (phazite::readID (element[schema::modulatorID]));
        const auto value = phazite::getModulatorValue (song, id);
        REQUIRE (value.has_value());
        CHECK (*value == *phazite::readModulatorValue (element, phazite::readModulatorPlace (element)));
        CHECK (*modulation.find (id) == *value);
    }

    CHECK (toVector (modulation.findList ({ .track = track })) == std::vector<ModulatorID> { lfo, steps, macro });
    CHECK (toVector (modulation.findList ({ .onMaster = true })) == std::vector<ModulatorID> { master });
    CHECK (phazite::getModulatorValue (song, master)->place.onMaster);
    CHECK (phazite::getModulatorTrack (song, master) == phazite::getMasterID (song));
    CHECK (phazite::isMacro (song, macro));
    CHECK_FALSE (phazite::isMacro (song, lfo));
    CHECK (phazite::hasModulations (song, track));
    CHECK_FALSE (phazite::getModulatorValue (song, ModulatorID::fromRaw (synth.getRaw())).has_value());

    // What modulates a parameter, by its modulators, without reading one whole.
    const auto before = phazite::countModulatorsRead();
    const auto cutoff = phazite::findModulationsOf (song, { synth, "cutoff" });
    CHECK (phazite::countModulatorsRead() == before);
    REQUIRE (cutoff.size() == 1);
    CHECK (cutoff[0].modulator == lfo);
    CHECK (cutoff[0].amount == doctest::Approx (0.25f));
}
