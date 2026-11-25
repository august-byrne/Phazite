// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/SectionsValue.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> sectionsRead { 0 }; // countSectionsRead(): a count alone, which orders nothing

    // Whether an ID is one the song can count on from (Schema.h: schema::maxID), as every item's is once the song is loaded.
    bool isHeldID (SectionID id)
    {
        return id.isValid() && id.getRaw() < schema::maxID;
    }

    // A section's energy as the song holds it: its number as the song reads one, within 0 to 1; its description's default
    // where it has none, or what it has is no finite number (the loader takes that away: model/project/internal/Project.h).
    float readHeldEnergy (const juce::var& held)
    {
        const auto number = readValueNumber (held);
        const auto& fallback = schema::sectionEnergy.getDefault();

        if (! number.has_value() || ! std::isfinite (*number))
            return fallback.isVoid() ? 0.5f : (float) static_cast<double> (fallback);

        return (float) juce::jlimit (0.0, 1.0, *number);
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
    return song.getValues()->sections.get();
}

bool holdsSection (const Song& song, SectionID id)
{
    if (! id.isValid())
        return false;

    // A look in the area's value as it is held, nothing copied: a paint asks this of each section in view.
    for (const auto& section : song.getValues()->sections.get().sections)
        if (section.id == id)
            return true;

    return false;
}

std::optional<SectionValue> readHeldSection (const juce::ValueTree& element)
{
    if (! element.hasType (schema::SECTION))
        return std::nullopt;

    SectionValue section;
    section.id = SectionID::fromRaw (readID (element[schema::sectionID]));

    if (! isHeldID (section.id))
        return std::nullopt;

    section.start = BeatPosition::fromBeats (readDescribed (element, schema::sectionStart));
    section.length = BeatDuration::fromBeats (readDescribed (element, schema::sectionLength));
    section.name = element[schema::sectionName].toString();
    section.role = element[schema::sectionRole].toString();
    section.tip = element[schema::sectionTip].toString();
    section.energy = readHeldEnergy (element[schema::sectionEnergy]);

    if (element.hasProperty (schema::sectionColour))
        section.colour = element[schema::sectionColour].toString();

    return section;
}

SectionsValue readHeldSections (const juce::ValueTree& sectionsElement)
{
    auto sections = immer::flex_vector<SectionValue>().transient();

    for (const auto& child : sectionsElement)
        if (auto section = readHeldSection (child))
            sections.push_back (std::move (*section));

    sectionsRead.fetch_add (sections.size(), std::memory_order_relaxed);
    return keepSectionsHeld ({ sections.persistent() });
}

SectionsValue keepSectionsHeld (const SectionsValue& value)
{
    std::set<SectionID> ids;
    auto kept = immer::flex_vector<SectionValue>().transient();
    auto changed = false; // a section dropped, or one kept within its ranges

    for (const auto& section : value.sections)
    {
        if (isHeldID (section.id) && ids.insert (section.id).second)
        {
            // Its start and its length within their descriptions' ranges (SongText.h: keepDescribed()), as the readers keep them.
            auto held = section;
            held.start = keepDescribed (section.start, schema::sectionStart);
            held.length = keepDescribed (section.length, schema::sectionLength);
            changed = changed || held.start != section.start || held.length != section.length;
            kept.push_back (std::move (held));
        }
        else
        {
            changed = true;
        }
    }

    return changed ? SectionsValue { kept.persistent() } : value;
}

bool holdsSectionsHeld (const SectionsValue& value)
{
    return keepSectionsHeld (value).isSameAs (value);
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

juce::ValueTree writeSectionElement (const SectionValue& section)
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

    return element;
}

void writeSections (const SectionsValue& value, juce::ValueTree& sectionsCopy)
{
    jassert (sectionsCopy.hasType (schema::SECTIONS));
    immer::for_each (value.sections,
                     [&sectionsCopy] (const SectionValue& section) { sectionsCopy.appendChild (writeSectionElement (section), nullptr); });
}
} // namespace phazite
