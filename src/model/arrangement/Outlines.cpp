// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Outlines.h"
#include "internal/Outlines.h"
#include "internal/Sections.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/transport/internal/Tempo.h"

#include <PhaziteData.h>

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr const char* outlineFileSuffix = ".outline.json";
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
            section.colour = findTrackColour (entry["colour"].toString());

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

void applyOutline (Song& song, const Outline& outline)
{
    removeSections (song);

    const auto bars = getBars (song);
    auto bar = 0;

    for (const auto& section : outline.sections)
    {
        const auto start = bars.getBarStart (bar);
        const auto end = bars.getBarStart (bar + section.bars);
        insertSection (song, start, { section.name, end - start, section.role, section.tip, section.energy, section.colour });
        bar += section.bars;
    }

    song.getState().setProperty (schema::outline, outline.id, &song.getUndoManager());
}

bool applyOutline (Song& song, const juce::String& outlineID)
{
    const auto* outline = findOutline (outlineID);

    if (outline == nullptr)
        return false;

    applyOutline (song, *outline);
    return true;
}

juce::String getAppliedOutline (const Song& song)
{
    return song.getState()[schema::outline].toString();
}

bool isOutlineApplied (const Song& song, const juce::String& outlineID)
{
    return getAppliedOutline (song) == outlineID && ! getSections (song).empty();
}

juce::String getOutlineCommandID (const juce::String& outlineID)
{
    return "section.outline." + outlineID;
}

//==============================================================================
void addOutlineCommands (CommandRegistry& registry, Selection& selection)
{
    // One command for each outline, so the palette finds them by name.
    for (const auto& outline : getBuiltInOutlines())
    {
        const auto id = outline.id;
        registry.add ({ .id = getOutlineCommandID (id),
                        .name = "Apply Outline: " + outline.name,
                        .category = "Section",
                        .description = "Lays out the song's sections. " + outline.description
                                       + " It replaces the sections there are; what is on the tracks stays where it is.",
                        .synonyms = { "song outline", "structure", "template", "arrangement", "form" },
                        .perform = withSong (selection, [id] (Song& song) { applyOutline (song, id); }),
                        .isEnabled = [&selection] { return selection.getSong() != nullptr; },
                        .undoTarget = undoTargetOf (selection),
                        .isTicked = ifSong (selection, [id] (const Song& song) { return isOutlineApplied (song, id); }) });
    }
}
} // namespace phazite
