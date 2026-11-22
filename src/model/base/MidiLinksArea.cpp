// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/MidiLinksArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <utility>

namespace phazite
{
namespace
{
    // The song's MIDILINKS element once what it held is read into the song's links (the adoption and the keeper's safety net
    // alike): emptied, the place a whole copy writes the links in, its properties and children Phazite doesn't know taken out
    // too (Schema.h, MIDILINKS).
    void emptyMidiLinksElement (juce::ValueTree& links, juce::UndoManager* undo)
    {
        links.removeAllChildren (undo);
        links.removeAllProperties (undo);
    }
} // namespace

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
SongValues::MidiLinksKeeper::MidiLinksKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

void SongValues::MidiLinksKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    const auto isLinks = child.hasType (schema::MIDILINKS) && parent == state;

    if (! isLinks && ! isHeardAsMidiLinks (parent, child))
        return;

    // The safety net (SongValues.h): a MIDILINKS that comes into the song with links in it has them read in as the song's
    // links, and is emptied as adopt() empties the song's own (emptyMidiLinksElement(): what Phazite doesn't know of it goes
    // too); a link added to the song's MIDILINKS by itself is read into the links and taken out again while the song is
    // written outside the history, and refused in a step, whose Undo, taking the tree's change back first, would find it gone
    // (JUCE workaround W62 (docs/juce-workarounds.md)). Either in the mode of the moment.
    auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

    if (isLinks)
    {
        auto links = 0;

        for (const auto& element : child)
            links += element.hasType (schema::MIDILINK) ? 1 : 0;

        if (links > 0)
        {
            values.midiLinksAdopted += links;
            values.setMidiLinks (song, readHeldMidiLinks (child));
        }

        // Emptied whether or not it held a link: what Phazite doesn't know of it goes too, so that it is the empty place.
        if (child.getNumChildren() > 0 || child.getNumProperties() > 0)
            emptyMidiLinksElement (child, undo);

        return;
    }

    if (! child.hasType (schema::MIDILINK) || ! parent.hasType (schema::MIDILINKS) || parent.getParent() != state)
        return;

    if (undo != nullptr)
    {
        jassertfalse; // a link added to the song's MIDI links: the MIDI links' functions change them (MidiLinks.h)
        return;
    }

    ++values.midiLinksAdopted;
    values.setMidiLinks (song, withLinkTaken (values.midiLinks.get(), child));
    parent.removeChild (child, nullptr);
}

void SongValues::MidiLinksKeeper::adopt (SongValues& into, Song& loaded)
{
    // The links the loader made consistent into their area, outside the history, each as the song holds one, one of each
    // controller and of each parameter (MidiLinksValue.h: readHeldMidiLinks()); then their element emptied
    // (emptyMidiLinksElement()), and kept: it is the place a whole copy writes the links in, written as the song had it. None
    // for a song without one.
    auto links = loaded.getState().getChildWithName (schema::MIDILINKS);

    if (! links.isValid())
        return;

    into.midiLinks.setOutsideHistory (readHeldMidiLinks (links));
    emptyMidiLinksElement (links, nullptr);
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

bool isHeardAsMidiLinks (const juce::ValueTree& element)
{
    for (auto each = element; each.isValid(); each = each.getParent())
        if (each.hasType (schema::MIDILINKS))
            return each.getParent().hasType (schema::PHAZITE);

    return false;
}

bool isHeardAsMidiLinks (const juce::ValueTree& parent, const juce::ValueTree&)
{
    return isHeardAsMidiLinks (parent);
}

//==============================================================================
bool leadsToMidiLinks (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::MIDILINKS);
}

void writeMidiLinksIn (juce::ValueTree& element, const ValuesKept& kept)
{
    // Nothing kept of no links, nor of a song that keeps everything in its tree (whose MIDILINKS holds its links).
    if (! element.hasType (schema::MIDILINKS) || kept.midiLinks.links.empty())
        return;

    jassert (element.getNumChildren() == 0); // the song's tree holds none of its links (SongValues.h)
    writeMidiLinks (kept.midiLinks, element);
}

bool holdSameMidiLinks (const juce::ValueTree& song, const ValuesKept& kept, const juce::ValueTree& otherSong, const ValuesKept& otherKept)
{
    // Whether each has a MIDILINKS (an empty one is written into the file, and is no song without one), and the values kept.
    if (song.getChildWithName (schema::MIDILINKS).isValid() != otherSong.getChildWithName (schema::MIDILINKS).isValid())
        return false;

    return kept.midiLinks.isSameAs (otherKept.midiLinks) || kept.midiLinks == otherKept.midiLinks;
}

bool holdSameMidiLinksOf (const juce::ValueTree& element, const ValuesKept& kept, const juce::ValueTree& otherElement,
                          const ValuesKept& otherKept)
{
    const auto hasMidiLinks = [] (const juce::ValueTree& each)
    { return each.hasType (schema::PHAZITE) && each.getChildWithName (schema::MIDILINKS).isValid(); };

    return ! (hasMidiLinks (element) || hasMidiLinks (otherElement)) || holdSameMidiLinks (element, kept, otherElement, otherKept);
}

bool isMidiLinksPlace (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return leadsToMidiLinks (parent, child);
}
} // namespace phazite
