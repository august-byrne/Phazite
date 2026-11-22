// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/Schema.h"

namespace phazite
{
bool MidiLinkEntry::operator== (const MidiLinkEntry& other) const noexcept
{
    return channel == other.channel && number == other.number && device == other.device && parameter == other.parameter;
}

MidiLinkEntry readMidiLinkEntry (const juce::ValueTree& link)
{
    return { (int) link[schema::midiLinkChannel], (int) link[schema::midiLinkNumber], readID (link[schema::midiLinkDevice]),
             link[schema::midiLinkParameter].toString() };
}

MidiLinksValue readMidiLinksValue (const juce::ValueTree& midiLinks)
{
    auto links = immer::flex_vector<MidiLinkEntry>().transient();

    for (const auto& link : midiLinks)
        if (link.hasType (schema::MIDILINK))
            links.push_back (readMidiLinkEntry (link));

    return { links.persistent() };
}

void writeMidiLinks (const MidiLinksValue& value, juce::ValueTree& midiLinks)
{
    jassert (midiLinks.hasType (schema::MIDILINKS));

    immer::for_each (value.links,
                     [&midiLinks] (const MidiLinkEntry& link)
                     {
                         midiLinks.appendChild (
                             juce::ValueTree (schema::MIDILINK, { { schema::midiLinkChannel, link.channel },
                                                                  { schema::midiLinkNumber, link.number },
                                                                  { schema::midiLinkDevice, juce::String ((juce::int64) link.device) },
                                                                  { schema::midiLinkParameter, link.parameter } }),
                             nullptr);
                     });
}
} // namespace phazite
