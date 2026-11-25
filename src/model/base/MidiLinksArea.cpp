// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/MidiLinksArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <utility>

namespace phazite
{
//==============================================================================
SongValues::MidiLinksTeller::MidiLinksTeller (ValueArea<MidiLinksValue>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::MidiLinksTeller::~MidiLinksTeller()
{
    area.removeListener (this);
}

void SongValues::MidiLinksTeller::valueAreaChanged (const MidiLinksValue& before, const MidiLinksValue& after)
{
    if (! before.isSameAs (after))
        listeners.call ([] (SongValues::Listener& listener) { listener.midiLinksChanged(); });
}

//==============================================================================
void readHeldMidiLinksIn (const juce::ValueTree& song, SongState& into)
{
    // The links the loader made consistent, each as the song holds one, one of each controller and of each parameter
    // (MidiLinksValue.h: readHeldMidiLinks()); none for a song without a MIDILINKS, or with an empty one (an empty list is
    // none).
    if (const auto links = song.getChildWithName (schema::MIDILINKS); links.isValid())
        into.midiLinks = readHeldMidiLinks (links);
}

//==============================================================================
void SongValues::setMidiLinks (Song& song, MidiLinksValue next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next == midiLinks.get())
        return;

    if (song.isWritingOutsideHistory())
        midiLinks.setOutsideHistory (std::move (next));
    else
        midiLinks.set (std::move (next));
}

//==============================================================================
bool leadsToMidiLinks (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::MIDILINKS);
}

void writeMidiLinksIn (juce::ValueTree& element, const SongState& kept)
{
    if (! element.hasType (schema::MIDILINKS) || kept.midiLinks.links.empty())
        return;

    writeMidiLinks (kept.midiLinks, element);
}

bool holdSameMidiLinks (const SongState& kept, const SongState& otherKept)
{
    // The values alone: none is none, an empty MIDILINKS being written nowhere (SongState.h).
    return kept.midiLinks.isSameAs (otherKept.midiLinks) || kept.midiLinks == otherKept.midiLinks;
}
} // namespace phazite
