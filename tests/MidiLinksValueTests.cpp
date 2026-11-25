// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's MIDI links as a value (model/base/internal/MidiLinksValue.h):
// the conversion between the value and the MIDILINKS element (the one place that reads and writes MIDILINK by name), round
// trip through the links of the golden files and through damaged ones; and the links' readers (model/midi/internal/
// MidiLinks.h) on the value, against what the song's elements hold.
//   phazite_tests.exe --test-case="MIDI links value*"
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/synth/Synth.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/internal/ProjectContext.h"

#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::MidiLinkEntry;
using phazite::MidiLinksValue;

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

juce::ValueTree writeLinks (const MidiLinksValue& value)
{
    juce::ValueTree written (schema::MIDILINKS);
    phazite::writeMidiLinks (value, written);
    return written;
}

juce::StringArray findElementDifferences (const juce::ValueTree& expected, const juce::ValueTree& actual)
{
    const auto a = expected.createXml(), b = actual.createXml();
    REQUIRE ((a != nullptr && b != nullptr));
    return phazite::test::findDifferences (*a, *b);
}
} // namespace

TEST_CASE ("MIDI links value: the golden project's links, read into a value and written into an emptied element, are the links "
           "they were")
{
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto* element = xml->getChildByName ("MIDILINKS");
    REQUIRE (element != nullptr);
    const auto links = juce::ValueTree::fromXml (*element);

    const auto value = phazite::readMidiLinksValue (links);
    REQUIRE (value.links.size() == 2);
    CHECK (value.links[0] == MidiLinkEntry { 3, 74, 15, "cutoff" });
    CHECK (value.links[1] == MidiLinkEntry { 1, 1, 95, "value" });

    const auto written = writeLinks (value);
    const auto differences = findElementDifferences (links, written);
    CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
    CHECK (phazite::readMidiLinksValue (written) == value);
    CHECK (written.getChild (0)[schema::midiLinkDevice].isString()); // a device's ID as text, as the links' code writes it
}

TEST_CASE ("MIDI links value: damaged links read as the links' code read them, in the element's order, and written as it writes "
           "them")
{
    // A link without a channel or a number, a device's ID as a number and as text with zeros before it, a child of another
    // type and a property Phazite doesn't know.
    const auto links = parse (R"(<MIDILINKS><MIDILINK device="12" parameter="cutoff"/><OTHER channel="2" number="3"/>)"
                              R"(<MIDILINK channel="16" number="119" device="0014" parameter="gain" colour="red"/>)"
                              R"(<MIDILINK channel="2" number="7" parameter="volume"/></MIDILINKS>)");
    auto withNumber = links.getChild (0);
    withNumber.setProperty (schema::midiLinkDevice, 12, nullptr);

    const auto value = phazite::readMidiLinksValue (links);
    REQUIRE (value.links.size() == 3);
    CHECK (value.links[0] == MidiLinkEntry { 0, 0, 12, "cutoff" });
    CHECK (value.links[1] == MidiLinkEntry { 16, 119, 14, "gain" });
    CHECK (value.links[2] == MidiLinkEntry { 2, 7, 0, "volume" });

    // Written: each link's four properties, its device's ID as the links' code writes one ("14"), read again the same.
    const auto written = writeLinks (value);
    REQUIRE (written.getNumChildren() == 3);
    CHECK (written.getChild (1)[schema::midiLinkDevice].toString() == "14");
    CHECK (written.getChild (1).getNumProperties() == 4);
    CHECK (phazite::readMidiLinksValue (written) == value);

    // An element of one link, read so; nothing for no element.
    CHECK (phazite::readMidiLinkEntry (links.getChild (2)) == value.links[1]);
    CHECK (phazite::readMidiLinksValue ({}).links.empty());
}

TEST_CASE ("MIDI links value: links read as the song holds them keep what their descriptions keep, one of each controller and of "
           "each parameter, in the element's order")
{
    // What enters the song's links (MidiLinksValue.h: readHeldMidiLinks()): a link without its controller, or with one that is no
    // number or outside its range (which brought within it would be another), goes, as the loader takes it away; so does one that
    // names no parameter; numbers written as text, or not whole, read as the loader reads them; a second link of a controller or
    // of a parameter goes, the first staying.
    const auto links = parse (R"(<MIDILINKS colour="red"><MIDILINK device="12" parameter="cutoff"/>)"
                              R"(<MIDILINK channel="17" number="3" device="12" parameter="gain"/>)"
                              R"(<MIDILINK channel="2" number="120" device="12" parameter="gain"/>)"
                              R"(<MIDILINK channel="nan" number="3" device="12" parameter="gain"/>)"
                              R"(<MIDILINK channel="2" number="3" parameter="gain"/>)"
                              R"(<MIDILINK channel="2" number="3" device="12"/>)"
                              R"(<MIDILINK channel=" 2 " number="6.6" device="0012" parameter="gain" colour="red"/><OTHER/>)"
                              R"(<MIDILINK channel="2" number="7" device="12" parameter="drive"/>)"
                              R"(<MIDILINK channel="3" number="1" device="12" parameter="gain"/>)"
                              R"(<MIDILINK channel="16" number="119" device="13" parameter="value"/></MIDILINKS>)");

    const auto value = phazite::readHeldMidiLinks (links);
    REQUIRE (value.links.size() == 2);
    CHECK (value.links[0] == MidiLinkEntry { 2, 7, 12, "gain" });
    CHECK (value.links[1] == MidiLinkEntry { 16, 119, 13, "value" });
    CHECK (phazite::holdsOneLinkEach (value));
    CHECK_FALSE (phazite::holdsOneLinkEach ({ value.links.push_back ({ 2, 7, 14, "cutoff" }) }));
    CHECK_FALSE (phazite::readHeldMidiLink (links.getChild (0)).has_value());
    CHECK_FALSE (phazite::readHeldMidiLink (links.getChild (7)).has_value()); // not a link
}

TEST_CASE ("MIDI links value: the song's links read through the value are the links its elements hold, a macro's by its ID")
{
    phazite::test::TestSong s;
    const auto synth = s.setInstrument (s.firstTrack, phazite::devices::Synth::typeID);
    const auto macro = s.addModulator (s.firstTrack, phazite::ModulatorType::macro);
    auto& song = *phazite::findSong (s.context);
    const phazite::ParameterRef macroValue { phazite::DeviceID::fromRaw (macro.getRaw()), juce::String (macro.getRaw()) };
    const auto volume = s.getVolumeParameter (s.firstTrack);

    {
        const phazite::UndoStep step (song, "Link MIDI Controllers");
        phazite::linkMidiController (song, { 4, 20 }, { synth, "cutoff" });
        phazite::linkMidiController (song, { 1, 3 }, macroValue);
        phazite::linkMidiController (song, { 2, 7 }, volume);
    }

    // The value, in the song's order, as the song whole holds them (what its file would).
    const auto value = phazite::getMidiLinksValue (song);
    REQUIRE (value.links.size() == 3);
    CHECK (value == phazite::readMidiLinksValue (phazite::test::copyWholeSong (song).getChildWithName (schema::MIDILINKS)));
    CHECK (value.links[1].parameter == juce::String (schema::midiLinkMacroValue)); // a macro's link names its value by the word

    // The readers: by channel, then number; a parameter's controller; a controller's parameter, a macro's by its ID.
    const auto links = phazite::getMidiLinks (song);
    REQUIRE (links.size() == 3);
    CHECK (links[0].controller == phazite::MidiController { 1, 3 });
    CHECK (links[0].parameter == macroValue);
    CHECK (links[1].controller == phazite::MidiController { 2, 7 });
    CHECK (links[1].parameter == volume);
    CHECK (links[2].controller == phazite::MidiController { 4, 20 });
    CHECK (phazite::findMidiLink (song, macroValue) == phazite::MidiController { 1, 3 });
    CHECK (phazite::findLinkedParameter (song, { 4, 20 }) == phazite::ParameterRef { synth, "cutoff" });
    CHECK_FALSE (phazite::findLinkedParameter (song, { 4, 21 }).has_value());
    CHECK (phazite::readMidiLinkParameter (song, value.links[1]) == macroValue);

    // Undone, the value has none.
    phazite::undo (song);
    CHECK (phazite::getMidiLinksValue (song).links.empty());
    CHECK (phazite::getMidiLinks (song).empty());
}
