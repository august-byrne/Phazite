// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Progressions.h"

#include <PhaziteData.h>

namespace phazite
{
namespace
{
    constexpr const char* progressionsFile = "progressions.json";

    juce::StringArray readTags (const juce::var& list)
    {
        juce::StringArray tags;

        if (const auto* entries = list.getArray())
            for (const auto& entry : *entries)
                if (const auto tag = entry.toString().trim(); tag.isNotEmpty())
                    tags.addIfNotAlreadyThere (tag);

        return tags;
    }

    template <typename GetTags>
    juce::StringArray collectTags (GetTags&& getTags)
    {
        juce::StringArray all;

        for (const auto& progression : getBuiltInProgressions())
            for (const auto& tag : getTags (progression))
                all.addIfNotAlreadyThere (tag);

        return all;
    }
} // namespace

double Progression::getLengthInBars() const
{
    auto bars = 0.0;

    for (const auto& chord : chords)
        bars += chord.bars;

    return bars;
}

juce::String Progression::describeNumerals() const
{
    juce::StringArray numerals;

    for (const auto& chord : chords)
        numerals.add (describeNumeral (chord.numeral));

    return numerals.joinIntoString (" - ");
}

std::optional<Progression> Progression::fromJSON (const juce::var& json)
{
    Progression progression;
    progression.id = json["id"].toString();
    progression.name = json["name"].toString();
    progression.feel = json["feel"].toString();
    progression.scale = findScale (json["scale"].toString()).value_or (Scale::major);
    progression.moods = readTags (json["moods"]);
    progression.genres = readTags (json["genres"]);

    if (progression.id.isEmpty() || progression.name.isEmpty())
        return std::nullopt;

    for (const auto& token : juce::StringArray::fromTokens (json["chords"].toString(), " ", ""))
    {
        if (token.isEmpty())
            continue;

        const auto star = token.indexOfChar ('*');
        const auto numeral = parseNumeral (star < 0 ? token : token.substring (0, star));
        const auto bars = star < 0 ? 1.0 : token.substring (star + 1).getDoubleValue();

        if (! numeral.has_value() || bars <= 0.0)
            return std::nullopt;

        progression.chords.push_back ({ *numeral, bars });
    }

    if (progression.chords.empty())
        return std::nullopt;

    return progression;
}

const std::vector<Progression>& getBuiltInProgressions()
{
    static const auto progressions = []
    {
        std::vector<Progression> found;
        int size = 0;

        for (int i = 0; i < PhaziteData::namedResourceListSize; ++i)
        {
            const auto* resource = PhaziteData::namedResourceList[i];

            if (juce::String (PhaziteData::getNamedResourceOriginalFilename (resource)) != progressionsFile)
                continue;

            const auto* data = PhaziteData::getNamedResource (resource, size);

            if (data == nullptr)
                continue;

            // Kept while its entries are read: they belong to it.
            const auto file = juce::JSON::parse (juce::String::fromUTF8 (data, size));

            if (const auto* entries = file["progressions"].getArray())
                for (const auto& entry : *entries)
                    if (auto progression = Progression::fromJSON (entry))
                        found.push_back (std::move (*progression));
        }

        return found;
    }();

    return progressions;
}

const Progression* findProgression (const juce::String& id)
{
    for (const auto& progression : getBuiltInProgressions())
        if (progression.id == id)
            return &progression;

    return nullptr;
}

juce::StringArray getProgressionMoods()
{
    return collectTags ([] (const Progression& p) { return p.moods; });
}

juce::StringArray getProgressionGenres()
{
    return collectTags ([] (const Progression& p) { return p.genres; });
}

std::vector<const Progression*> findProgressions (const juce::String& mood, const juce::String& genre)
{
    std::vector<const Progression*> found;

    for (const auto& progression : getBuiltInProgressions())
        if ((mood.isEmpty() || progression.moods.contains (mood)) && (genre.isEmpty() || progression.genres.contains (genre)))
            found.push_back (&progression);

    return found;
}

juce::StringArray getChordNamesIn (const Progression& progression, const Key& key)
{
    const auto progressionKey = getKeyForProgression (key, progression.scale);
    juce::StringArray names;

    for (const auto& chord : progression.chords)
        names.add (getChordNameIn (chord.numeral, progressionKey));

    return names;
}
} // namespace phazite
