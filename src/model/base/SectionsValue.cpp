// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/SectionsValue.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"

#include <atomic>
#include <cstdint>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> sectionsRead { 0 }; // countSectionsRead(): a count alone, which orders nothing

    // Each property a section has read as setSectionProperty() reads it, absent as a void value.
    SectionValue readSection (const juce::ValueTree& element)
    {
        SectionValue section;

        for (const auto* property : { &schema::sectionID.getName(), &schema::sectionStart.getName(), &schema::sectionLength.getName(),
                                      &schema::sectionName.getName(), &schema::sectionRole.getName(), &schema::sectionTip.getName(),
                                      &schema::sectionEnergy.getName(), &schema::sectionColour.getName() })
            setSectionProperty (section, *property, element[*property]);

        return section;
    }
} // namespace

bool SectionValue::operator== (const SectionValue& other) const noexcept
{
    return id == other.id && start == other.start && length == other.length && name == other.name && role == other.role && tip == other.tip
           && juce::exactlyEqual (energy, other.energy) && colour == other.colour;
}

const SectionValue* SectionsValue::find (SectionID id) const
{
    for (const auto& section : sections)
        if (section.id == id)
            return &section;

    return nullptr;
}

SectionsValue getSectionsValue (const Song& song)
{
    return readSectionsValue (song.getState().getChildWithName (schema::SECTIONS));
}

bool holdsSection (const Song& song, SectionID id)
{
    if (! id.isValid())
        return false;

    // Each SECTION's ID compared where the tree holds it, read as the value reads it (setSectionProperty()), nothing copied.
    for (const auto& child : song.getState().getChildWithName (schema::SECTIONS))
        if (child.hasType (schema::SECTION) && SectionID::fromRaw ((std::uint64_t) (juce::int64) child[schema::sectionID]) == id)
            return true;

    return false;
}

SectionsValue readSectionsValue (const juce::ValueTree& sectionsElement)
{
    auto sections = immer::flex_vector<SectionValue>().transient();

    for (const auto& child : sectionsElement)
        if (child.hasType (schema::SECTION))
            sections.push_back (readSection (child));

    sectionsRead.fetch_add (sections.size(), std::memory_order_relaxed);
    return { sections.persistent() };
}

std::uint64_t countSectionsRead() noexcept
{
    return sectionsRead.load (std::memory_order_relaxed);
}

bool setSectionProperty (SectionValue& section, const juce::Identifier& property, const juce::var& value)
{
    if (property == schema::sectionID)
        section.id = SectionID::fromRaw ((std::uint64_t) (juce::int64) value);
    else if (property == schema::sectionStart)
        section.start = BeatPosition::fromBeats ((double) value);
    else if (property == schema::sectionLength)
        section.length = BeatDuration::fromBeats ((double) value);
    else if (property == schema::sectionName)
        section.name = value.toString();
    else if (property == schema::sectionRole)
        section.role = value.toString();
    else if (property == schema::sectionTip)
        section.tip = value.toString();
    else if (property == schema::sectionEnergy)
        section.energy = juce::jlimit (0.0f, 1.0f, value.isVoid() ? 0.5f : (float) static_cast<double> (value));
    else if (property == schema::sectionColour)
        section.colour = value.isVoid() ? std::nullopt : std::optional<juce::String> (value.toString());
    else
        return false;

    return true;
}

void writeSections (const SectionsValue& value, juce::ValueTree& sectionsCopy)
{
    jassert (sectionsCopy.hasType (schema::SECTIONS));

    immer::for_each (value.sections,
                     [&sectionsCopy] (const SectionValue& section)
                     {
                         juce::ValueTree element (schema::SECTION, { { schema::sectionID, (juce::int64) section.id.getRaw() },
                                                                     { schema::sectionStart, section.start.inBeats() },
                                                                     { schema::sectionLength, section.length.inBeats() },
                                                                     { schema::sectionName, section.name } });

                         if (section.role.isNotEmpty())
                             element.setProperty (schema::sectionRole, section.role, nullptr);

                         if (section.tip.isNotEmpty())
                             element.setProperty (schema::sectionTip, section.tip, nullptr);

                         element.setProperty (schema::sectionEnergy, section.energy, nullptr);

                         if (section.colour.has_value())
                             element.setProperty (schema::sectionColour, *section.colour, nullptr);

                         sectionsCopy.appendChild (element, nullptr);
                     });
}
} // namespace phazite
