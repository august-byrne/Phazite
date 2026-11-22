// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    // A link's controller number as the MIDI links' code has always read one (readMidiLinksValue()): its text read as the song
    // reads a number (SongText.h: readValueNumber()), truncated to a whole one, 0 where it has none.
    int readWholeOrZero (const juce::var& held)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? toWhole<int> (*number) : 0;
    }

    // A link's controller number as the song holds one (readHeldMidiLinks()): by its description, made whole, within its
    // range; none where it is absent, no finite number, or outside (the description's goingOutside(): the link goes).
    std::optional<int> readHeldController (const juce::ValueTree& link, const schema::PropertyDescription& property)
    {
        const auto number = readValueNumber (link[property]);
        const auto& range = property.getRange();

        if (! number.has_value() || ! std::isfinite (*number) || ! range.has_value())
            return std::nullopt;

        const auto whole = std::round (*number);

        if (whole < range->low || whole > range->high)
            return std::nullopt;

        return roundToWhole<int> (whole);
    }

    // The rule of keepOneLinkEach(), a link at a time in the links' order: whether it is the first of its controller and the
    // first of its parameter.
    struct OneLinkEach
    {
        std::set<std::pair<int, int>> controllers;
        std::set<std::pair<std::uint64_t, juce::String>> parameters;

        bool keeps (const MidiLinkEntry& link)
        {
            const std::pair<int, int> byController { link.channel, link.number };
            std::pair<std::uint64_t, juce::String> byParameter { link.device, link.parameter };

            if (controllers.contains (byController) || parameters.contains (byParameter))
                return false;

            controllers.insert (byController);
            parameters.insert (std::move (byParameter));
            return true;
        }
    };
} // namespace

bool MidiLinkEntry::operator== (const MidiLinkEntry& other) const noexcept
{
    return channel == other.channel && number == other.number && device == other.device && parameter == other.parameter;
}

MidiLinkEntry readMidiLinkEntry (const juce::ValueTree& link)
{
    return { readWholeOrZero (link[schema::midiLinkChannel]), readWholeOrZero (link[schema::midiLinkNumber]),
             readID (link[schema::midiLinkDevice]), link[schema::midiLinkParameter].toString() };
}

MidiLinksValue readMidiLinksValue (const juce::ValueTree& midiLinks)
{
    auto links = immer::flex_vector<MidiLinkEntry>().transient();

    for (const auto& link : midiLinks)
        if (link.hasType (schema::MIDILINK))
            links.push_back (readMidiLinkEntry (link));

    return { links.persistent() };
}

std::optional<MidiLinkEntry> readHeldMidiLink (const juce::ValueTree& link)
{
    if (! link.hasType (schema::MIDILINK))
        return std::nullopt;

    const auto channel = readHeldController (link, schema::midiLinkChannel);
    const auto number = readHeldController (link, schema::midiLinkNumber);
    MidiLinkEntry entry { channel.value_or (0), number.value_or (0), readID (link[schema::midiLinkDevice]),
                          link[schema::midiLinkParameter].toString() };

    if (! channel.has_value() || ! number.has_value() || entry.device == 0 || entry.parameter.isEmpty())
        return std::nullopt;

    return entry;
}

MidiLinksValue readHeldMidiLinks (const juce::ValueTree& midiLinks)
{
    auto links = immer::flex_vector<MidiLinkEntry>().transient();

    for (const auto& link : midiLinks)
        if (auto entry = readHeldMidiLink (link))
            links.push_back (std::move (*entry));

    return keepOneLinkEach ({ links.persistent() });
}

MidiLinksValue withLinkTaken (const MidiLinksValue& value, const juce::ValueTree& link)
{
    if (auto entry = readHeldMidiLink (link))
        return keepOneLinkEach ({ value.links.push_back (std::move (*entry)) });

    return value;
}

MidiLinksValue keepOneLinkEach (const MidiLinksValue& value)
{
    OneLinkEach rule;
    auto kept = immer::flex_vector<MidiLinkEntry>().transient();
    auto dropped = false;

    for (const auto& link : value.links)
    {
        if (rule.keeps (link))
            kept.push_back (link);
        else
            dropped = true;
    }

    return dropped ? MidiLinksValue { kept.persistent() } : value;
}

bool holdsOneLinkEach (const MidiLinksValue& value)
{
    return keepOneLinkEach (value).isSameAs (value);
}

juce::ValueTree writeMidiLinkElement (const MidiLinkEntry& link)
{
    return juce::ValueTree (schema::MIDILINK, { { schema::midiLinkChannel, link.channel },
                                                { schema::midiLinkNumber, link.number },
                                                { schema::midiLinkDevice, juce::String ((juce::int64) link.device) },
                                                { schema::midiLinkParameter, link.parameter } });
}

void writeMidiLinks (const MidiLinksValue& value, juce::ValueTree& midiLinks)
{
    jassert (midiLinks.hasType (schema::MIDILINKS));
    immer::for_each (value.links,
                     [&midiLinks] (const MidiLinkEntry& link) { midiLinks.appendChild (writeMidiLinkElement (link), nullptr); });
}
} // namespace phazite
