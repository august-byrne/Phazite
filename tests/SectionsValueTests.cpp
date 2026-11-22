// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections as a value (model/base/internal/SectionsValue.h; roadmap step S5.20, the sections' first piece): the
// conversion between the value and the SECTIONS element (the one place that reads and writes SECTION by name), round trip
// through the sections of the golden files and through damaged ones; a merge's clash on a section's property; and the
// sections' readers on the value (model/arrangement/internal/Sections.h, the selection's), the same as before it came.
//   phazite_tests.exe --test-case="Sections value*"
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "model/arrangement/internal/Sections.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/Selection.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::SectionsValue;
using phazite::SectionValue;

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

juce::ValueTree writeSections (const SectionsValue& value)
{
    juce::ValueTree written (schema::SECTIONS);
    phazite::writeSections (value, written);
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

// A section as the sections' code made it of its element before the value came (Sections.cpp, as it was at commit 514dba6e).
phazite::Section makeSectionAsBefore (const juce::ValueTree& state)
{
    phazite::Section section;
    section.id = phazite::SectionID::fromRaw ((std::uint64_t) (juce::int64) state[schema::sectionID]);
    const auto start = beat (state[schema::sectionStart]);
    section.range = { start, start + phazite::BeatDuration::fromBeats (state[schema::sectionLength]) };
    section.name = state[schema::sectionName].toString();
    section.role = state[schema::sectionRole].toString();
    section.tip = state[schema::sectionTip].toString();
    section.energy = juce::jlimit (0.0f, 1.0f, (float) static_cast<double> (state.getProperty (schema::sectionEnergy, 0.5)));
    section.colour = state.hasProperty (schema::sectionColour) ? juce::Colour::fromString (state[schema::sectionColour].toString())
                                                               : phazite::getRoleColour (section.role);
    return section;
}

// The sections' readers on a song against what its SECTIONS element read as before gives: every section in song order (by
// their starts, two at one start in the element's order), each by its ID, and where the last ends.
void checkReadersAsBefore (const phazite::Song& song, const juce::ValueTree& sections)
{
    std::vector<phazite::Section> before;

    for (const auto& state : sections)
        if (state.hasType (schema::SECTION))
            before.push_back (makeSectionAsBefore (state));

    std::stable_sort (before.begin(), before.end(),
                      [] (const phazite::Section& a, const phazite::Section& b) { return a.range.getStart() < b.range.getStart(); });

    const auto now = phazite::getSections (song);
    REQUIRE (now.size() == before.size());
    phazite::BeatPosition end;

    for (size_t i = 0; i < now.size(); ++i)
    {
        INFO ("section " << i);
        CHECK (now[i].id == before[i].id);
        CHECK (now[i].range == before[i].range);
        CHECK (now[i].name == before[i].name);
        CHECK (now[i].role == before[i].role);
        CHECK (now[i].tip == before[i].tip);
        CHECK (juce::exactlyEqual (now[i].energy, before[i].energy));
        CHECK (now[i].colour == before[i].colour);
        end = std::max (end, before[i].range.getEnd());

        // By its ID: the first with it in the element's order (none for none).
        for (const auto& state : sections)
        {
            if (before[i].id.isValid() && state.hasType (schema::SECTION) && makeSectionAsBefore (state).id == before[i].id)
            {
                const auto found = phazite::getSection (song, before[i].id);
                REQUIRE (found.has_value());
                CHECK (found->range == makeSectionAsBefore (state).range);
                CHECK (found->name == makeSectionAsBefore (state).name);
                break;
            }
        }
    }

    CHECK (phazite::getSectionsEnd (song) == end);
    CHECK_FALSE (phazite::getSection (song, phazite::SectionID()).has_value());
}
} // namespace

TEST_CASE ("Sections value: the golden files' sections, read into a value and written into an emptied element, are the sections "
           "they were")
{
    for (const auto* name : { "projects/representative.xml", "commands/script.xml" })
    {
        INFO (juce::String (name));
        const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile (name));
        REQUIRE (xml != nullptr);
        const auto* element = xml->getChildByName ("SECTIONS");
        REQUIRE (element != nullptr);
        const auto sections = juce::ValueTree::fromXml (*element);

        const auto value = phazite::readSectionsValue (sections);
        REQUIRE ((int) value.sections.size() == sections.getNumChildren());
        const auto written = writeSections (value);
        const auto differences = findElementDifferences (sections, written);
        CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
        CHECK (phazite::readSectionsValue (written) == value);

        // Read as the song holds them (SectionsValue.h: readHeldSections(), what the song takes in as it opens): the same.
        CHECK (phazite::readHeldSections (sections) == value);
    }
}

TEST_CASE ("Sections value: damaged sections read as the sections' code read them, written as it writes them, and the readers read "
           "them as before")
{
    // A section without an ID, a start or a length; an energy beyond its range and none; a colour written empty and none; two
    // at one start; two with one ID; a child of another type and a property Phazite doesn't know.
    const auto sections = parse (R"(<SECTIONS>)"
                                 R"(<SECTION name="Nothing"/>)"
                                 R"(<SECTION id="9" start="16" length="8" name="Late" role="drop" energy="3" colour=""/>)"
                                 R"(<OTHER id="12" start="2"/>)"
                                 R"(<SECTION id="10" start="4" length="4" name="First at 4" role="verse" tip="Hm." colour="ff112233"/>)"
                                 R"(<SECTION id="11" start="4" length="2" name="Second at 4" energy="-1" shape="round"/>)"
                                 R"(<SECTION id="9" start="40" length="4" name="Same ID"/>)"
                                 R"(</SECTIONS>)");

    const auto value = phazite::readSectionsValue (sections);
    REQUIRE (value.sections.size() == 5);
    const auto& nothing = value.sections[0];
    CHECK_FALSE (nothing.id.isValid());
    CHECK (nothing.start == beat (0.0));
    CHECK (nothing.length == phazite::BeatDuration());
    CHECK (juce::exactlyEqual (nothing.energy, 0.5f));
    CHECK_FALSE (nothing.colour.has_value());
    CHECK (juce::exactlyEqual (value.sections[1].energy, 1.0f));
    CHECK (value.sections[1].colour == std::optional<juce::String> (juce::String())); // written empty: empty, not none
    CHECK (value.sections[2].colour == std::optional<juce::String> ("ff112233"));
    CHECK (value.sections[2].tip == "Hm.");
    CHECK (juce::exactlyEqual (value.sections[3].energy, 0.0f));
    CHECK (value.find (phazite::SectionID::fromRaw (9)) == &value.sections[1]);
    CHECK (value.find (phazite::SectionID::fromRaw (12)) == nullptr);

    // Written as the sections' code writes them (a role, a tip and a colour only where there is one), read again the same.
    const auto written = writeSections (value);
    CHECK (phazite::readSectionsValue (written) == value);
    CHECK_FALSE (written.getChild (0).hasProperty (schema::sectionRole));
    CHECK_FALSE (written.getChild (0).hasProperty (schema::sectionColour));
    CHECK (written.getChild (0).hasProperty (schema::sectionName));
    CHECK (phazite::readSectionsValue ({}) == SectionsValue {});

    // The readers on a song that keeps everything in its tree (one made to be read or checked, without values: as a file from
    // others would be before the loader makes it consistent), the same as the sections' code read them before.
    auto raw = phazite::makeNewSong();
    raw.appendChild (sections.createCopy(), nullptr);
    const phazite::Song rawSong { raw };
    REQUIRE (rawSong.getValues() == nullptr);
    CHECK (phazite::getSectionsValue (rawSong) == value);
    checkReadersAsBefore (rawSong, sections);

    // And on a song opened with them (a file from others, which the loader makes consistent and the song takes into its
    // values: SectionsValue.h, readHeldSections()), the same as the sections' code read the sections the loader left.
    auto state = phazite::makeNewSong();
    state.appendChild (sections.createCopy(), nullptr);
    const auto song = phazite::makeSong (state);
    REQUIRE (song != nullptr);
    const auto held = phazite::test::copyWholeSong (*song).getChildWithName (schema::SECTIONS);
    CHECK (phazite::getSectionsValue (*song) == phazite::readSectionsValue (held));
    CHECK_FALSE (phazite::getSectionsValue (*song).sections.empty());
    checkReadersAsBefore (*song, held);
}

TEST_CASE ("Sections value: sections read as the song holds them keep what their descriptions keep, one of each ID, in the "
           "element's order")
{
    // What enters the song's sections (SectionsValue.h: readHeldSections(), withSectionTaken()): a section without an ID the
    // song can count on from goes, as does a second of one ID (the first stays); a start and a length by their descriptions
    // (within their ranges, and their fallbacks where they have none or no finite number: a length of four beats); an energy
    // within 0 to 1, 0.5 where it has none or no number; a colour as written, none where it has none.
    const auto sections = parse (R"(<SECTIONS colour="red"><SECTION name="No ID" start="0" length="4"/>)"
                                 R"(<SECTION id="0" name="Zero" start="0" length="4"/>)"
                                 R"(<SECTION id="9" name="Damaged" start="-4" energy="nan"/><OTHER id="11"/>)"
                                 R"(<SECTION id="9" name="Again" start="40" length="4"/>)"
                                 R"(<SECTION id=" 10 " name="Text" start=" 2 " length="-3" energy="7" colour="" tip="Hm."/>)"
                                 R"(</SECTIONS>)");

    const auto value = phazite::readHeldSections (sections);
    REQUIRE (value.sections.size() == 2);
    const auto& damaged = value.sections[0];
    CHECK (damaged.id == phazite::SectionID::fromRaw (9));
    CHECK (damaged.name == "Damaged");
    CHECK (damaged.start == beat (0.0));
    CHECK (damaged.length == phazite::BeatDuration::fromBeats (4.0));
    CHECK (juce::exactlyEqual (damaged.energy, 0.5f));
    CHECK_FALSE (damaged.colour.has_value());
    const auto& text = value.sections[1];
    CHECK (text.id == phazite::SectionID::fromRaw (10));
    CHECK (text.start == beat (2.0));
    CHECK (text.length > phazite::BeatDuration());
    CHECK (juce::exactlyEqual (text.energy, 1.0f));
    CHECK (text.colour == std::optional<juce::String> (juce::String()));
    CHECK (text.tip == "Hm.");
    CHECK (phazite::holdsSectionsHeld (value));
    CHECK_FALSE (phazite::holdsSectionsHeld ({ value.sections.push_back (damaged) }));
    CHECK_FALSE (phazite::readHeldSection (sections.getChild (0)).has_value());
    CHECK_FALSE (phazite::readHeldSection (sections.getChild (3)).has_value()); // not a section

    // A section taken in after the others: kept where it is the first of its ID.
    SectionValue bridge;
    bridge.id = phazite::SectionID::fromRaw (12);
    bridge.name = "Bridge";
    const auto taken = phazite::withSectionTaken (value, phazite::writeSectionElement (bridge));
    REQUIRE (taken.sections.size() == 3);
    CHECK (taken.sections[2] == bridge);
    CHECK (phazite::withSectionTaken (value, phazite::writeSectionElement (damaged)) == value);
    CHECK (phazite::withSectionTaken (value, juce::ValueTree ("OTHER")).isSameAs (value));
}

TEST_CASE ("Sections value: a colour written empty and none round-trip as they were, and show as they did")
{
    // One written empty stays empty (not "0", which juce::Colour().toString() is); one without stays without. The readers show
    // the first as juce::Colour::fromString() reads "" (transparent black), the second in its role's colour.
    const auto sections = parse (R"(<SECTIONS>)"
                                 R"(<SECTION id="3" start="0" length="4" name="Empty" role="drop" energy="0.5" colour=""/>)"
                                 R"(<SECTION id="4" start="4" length="4" name="None" role="drop" energy="0.5"/>)"
                                 R"(</SECTIONS>)");

    const auto value = phazite::readSectionsValue (sections);
    REQUIRE (value.sections.size() == 2);
    CHECK (value.sections[0].colour == std::optional<juce::String> (juce::String()));
    CHECK_FALSE (value.sections[1].colour.has_value());

    const auto written = writeSections (value);
    REQUIRE (written.getNumChildren() == 2);
    CHECK (written.getChild (0).hasProperty (schema::sectionColour));
    CHECK (written.getChild (0)[schema::sectionColour].toString() == juce::String());
    CHECK_FALSE (written.getChild (1).hasProperty (schema::sectionColour));
    const auto differences = findElementDifferences (sections, written);
    CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));

    // A song opened with them (the loader leaves both colours as they are, and may give the sections other IDs, which the
    // song's own items have).
    auto state = phazite::makeNewSong();
    state.appendChild (sections.createCopy(), nullptr);
    const auto song = phazite::makeSong (state);
    REQUIRE (song != nullptr);
    checkReadersAsBefore (*song, phazite::test::copyWholeSong (*song).getChildWithName (schema::SECTIONS));
    const auto shown = phazite::getSections (*song);
    REQUIRE (shown.size() == 2);
    CHECK (shown[0].colour == juce::Colour());
    CHECK (shown[1].colour == phazite::getRoleColour ("drop"));
}

TEST_CASE ("Sections value: a merge's clash sets each property of a section as the element held it, and nothing for a property "
           "that isn't a section's")
{
    SectionValue section;
    section.id = phazite::SectionID::fromRaw (5);
    section.start = beat (8.0);
    section.length = phazite::BeatDuration::fromBeats (16.0);
    section.name = "Verse";
    section.role = "verse";
    section.tip = "Tell the story.";
    section.energy = 0.25f;
    section.colour = "ff445566";

    // Each property given a value, taken away (a void value) and, for a number, given one beyond its range: what the section
    // then holds, written out by hand (an element that held it would read as this).
    struct Case
    {
        const juce::Identifier* property;
        juce::var given;
        std::function<void (SectionValue&)> expect;
    };

    const auto& id = schema::sectionID.getName();
    const auto& start = schema::sectionStart.getName();
    const auto& length = schema::sectionLength.getName();
    const auto& energy = schema::sectionEnergy.getName();
    const auto& colour = schema::sectionColour.getName();

    const std::vector<Case> cases {
        { &id, 7, [] (SectionValue& s) { s.id = phazite::SectionID::fromRaw (7); } },
        { &id, juce::var(), [] (SectionValue& s) { s.id = phazite::SectionID(); } },
        { &start, 12.5, [] (SectionValue& s) { s.start = beat (12.5); } },
        { &start, "3", [] (SectionValue& s) { s.start = beat (3.0); } },
        { &start, -4.0, [] (SectionValue& s) { s.start = beat (-4.0); } }, // the loader keeps it within its range
        { &start, juce::var(), [] (SectionValue& s) { s.start = beat (0.0); } },
        { &length, 2.0, [] (SectionValue& s) { s.length = phazite::BeatDuration::fromBeats (2.0); } },
        { &length, -2.0, [] (SectionValue& s) { s.length = phazite::BeatDuration::fromBeats (-2.0); } },
        { &length, juce::var(), [] (SectionValue& s) { s.length = phazite::BeatDuration(); } },
        { &schema::sectionName.getName(), "Chorus", [] (SectionValue& s) { s.name = "Chorus"; } },
        { &schema::sectionName.getName(), "", [] (SectionValue& s) { s.name = juce::String(); } },
        { &schema::sectionName.getName(), juce::var(), [] (SectionValue& s) { s.name = juce::String(); } },
        { &schema::sectionRole.getName(), "chorus", [] (SectionValue& s) { s.role = "chorus"; } },
        { &schema::sectionRole.getName(), juce::var(), [] (SectionValue& s) { s.role = juce::String(); } },
        { &schema::sectionTip.getName(), "Lift it.", [] (SectionValue& s) { s.tip = "Lift it."; } },
        { &schema::sectionTip.getName(), juce::var(), [] (SectionValue& s) { s.tip = juce::String(); } },
        { &energy, 0.75, [] (SectionValue& s) { s.energy = 0.75f; } },
        { &energy, 4.0, [] (SectionValue& s) { s.energy = 1.0f; } },
        { &energy, -2.0, [] (SectionValue& s) { s.energy = 0.0f; } },
        { &energy, juce::var(), [] (SectionValue& s) { s.energy = 0.5f; } },
        { &colour, "ff000000", [] (SectionValue& s) { s.colour = juce::String ("ff000000"); } },
        { &colour, "", [] (SectionValue& s) { s.colour = juce::String(); } },
        { &colour, juce::var(), [] (SectionValue& s) { s.colour = std::nullopt; } },
    };

    for (const auto& each : cases)
    {
        INFO (each.property->toString() << " = " << each.given.toString() << (each.given.isVoid() ? " (taken away)" : ""));
        auto set = section;
        auto expected = section;
        each.expect (expected);
        REQUIRE (phazite::setSectionProperty (set, *each.property, each.given));
        CHECK (set == expected);
    }

    auto unchanged = section;
    CHECK_FALSE (phazite::setSectionProperty (unchanged, juce::Identifier ("shape"), "round"));
    CHECK (unchanged == section);
}

TEST_CASE ("Sections value: the sections the model made are read through the value, by the selection and the readers alike")
{
    // The selection asks the song whether a section is there (Selection.cpp: isThere()): one that found none would leave
    // no section selected.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& selection = phazite::getSelection (s.context);
    phazite::SectionID verse, chorus;

    {
        const phazite::UndoStep step (song, "Sections");
        verse = phazite::insertSection (song, beat (0.0), { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) });
        chorus = phazite::insertSection (song, beat (16.0),
                                         { .name = "Chorus", .length = phazite::BeatDuration::fromBeats (8.0), .role = "chorus" });
    }

    const auto sections = phazite::test::copyWholeSong (song).getChildWithName (schema::SECTIONS);
    CHECK (phazite::getSectionsValue (song) == phazite::readSectionsValue (sections));
    checkReadersAsBefore (song, sections);

    selection.selectOnly (verse);
    CHECK (selection.isSelected (verse));
    selection.addToSelection (chorus);
    CHECK (selection.isSelected (chorus));
    CHECK (selection.getNumObjectsSelected() == 2);
    CHECK (phazite::getIDs (phazite::getSelectedSections (selection)) == std::vector { verse, chorus });
    CHECK (phazite::hasSelectedSections (selection));

    // Gone (undone), neither is selected.
    phazite::undo (song);
    CHECK_FALSE (selection.isSelected (verse));
    CHECK (selection.getNumObjectsSelected() == 0);
    CHECK_FALSE (phazite::hasSelectedSections (selection));
}
