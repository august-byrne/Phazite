// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Outlines.h"
#include "Sections.h"
#include "Tracks.h"
#include "model/base/Schema.h"

#include <PhaziteData.h>

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr const char* outlineFileSuffix = ".outline.json";

    std::optional<juce::Colour> findPaletteColour (const juce::String& name)
    {
        for (const auto& colour : getTrackColours())
            if (name.equalsIgnoreCase (colour.name))
                return colour.colour;

        return {};
    }
} // namespace

int Outline::getLengthInBars() const
{
    auto bars = 0;

    for (const auto& section : sections)
        bars += section.bars;

    return bars;
}

std::optional<Outline> Outline::fromJSON (const juce::String& text)
{
    const auto json = juce::JSON::parse (text);
    Outline outline;
    outline.id = json["id"].toString();
    outline.name = json["name"].toString();
    outline.description = json["description"].toString();
    outline.order = json.hasProperty ("order") ? static_cast<int> (json["order"]) : outline.order;

    if (outline.id.isEmpty() || outline.name.isEmpty())
        return {};

    if (auto* entries = json["sections"].getArray())
    {
        for (const auto& entry : *entries)
        {
            Section section;
            section.name = entry["name"].toString();
            section.role = entry["role"].toString();
            section.tip = entry["tip"].toString();
            section.bars = static_cast<int> (entry["bars"]);
            section.energy = juce::jlimit (0.0f, 1.0f, (float) static_cast<double> (entry.getProperty ("energy", 0.5)));
            section.colour = findPaletteColour (entry["colour"].toString());

            if (section.name.isEmpty() || section.bars <= 0)
                return {};

            outline.sections.push_back (std::move (section));
        }
    }

    if (outline.sections.empty())
        return {};

    return outline;
}

const std::vector<Outline>& getBuiltInOutlines()
{
    // Every compiled-in file whose name ends in .outline.json.
    static const auto outlines = []
    {
        std::vector<Outline> found;

        for (int i = 0; i < PhaziteData::namedResourceListSize; ++i)
        {
            const auto* resource = PhaziteData::namedResourceList[i];

            if (! juce::String (PhaziteData::getNamedResourceOriginalFilename (resource)).endsWith (outlineFileSuffix))
                continue;

            int size = 0;

            if (const auto* data = PhaziteData::getNamedResource (resource, size))
                if (auto outline = Outline::fromJSON (juce::String::fromUTF8 (data, size)))
                    found.push_back (std::move (*outline));
        }

        std::stable_sort (found.begin(), found.end(), [] (const Outline& a, const Outline& b) { return a.order < b.order; });
        return found;
    }();

    return outlines;
}

const Outline* findOutline (const juce::String& id)
{
    for (const auto& outline : getBuiltInOutlines())
        if (outline.id == id)
            return &outline;

    return nullptr;
}

void applyOutline (te::Edit& edit, const Outline& outline)
{
    for (auto* section : getSections (edit))
        section->removeFromParent();

    const auto& tempo = edit.tempoSequence;
    auto bar = 0;

    for (const auto& section : outline.sections)
    {
        const auto start = tempo.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 });
        const auto end = tempo.toBeats (te::tempo::BarsAndBeats { bar + section.bars, {}, 0 });
        insertSection (edit, start, { section.name, end - start, section.role, section.tip, section.energy, section.colour });
        bar += section.bars;
    }

    getPhaziteState (edit).setProperty (schema::outline, outline.id, &edit.getUndoManager());
}

juce::String getAppliedOutline (const te::Edit& edit)
{
    return findPhaziteState (edit)[schema::outline].toString();
}
} // namespace phazite
