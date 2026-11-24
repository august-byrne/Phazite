// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SectionsArea.h"
#include "Song.h"
#include "internal/IDsHeld.h"
#include "internal/Schema.h"

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    // The song's SECTIONS element once what it held is read into the song's sections (the adoption and the keeper's safety net
    // alike): emptied, the place a whole copy writes the sections in, its properties and children Phazite doesn't know taken
    // out too (Schema.h, SECTIONS).
    void emptySectionsElement (juce::ValueTree& sections, juce::UndoManager* undo)
    {
        sections.removeAllChildren (undo);
        sections.removeAllProperties (undo);
    }

    bool contains (const std::vector<SectionID>& ids, SectionID id)
    {
        return std::find (ids.begin(), ids.end(), id) != ids.end();
    }
} // namespace

//==============================================================================
SongValues::SectionsTeller::SectionsTeller (ValueArea<SectionsValue>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::SectionsTeller::~SectionsTeller()
{
    area.removeListener (this);
}

void SongValues::SectionsTeller::valueAreaChanged (const SectionsValue& before, const SectionsValue& after)
{
    if (before.isSameAs (after))
        return;

    const auto changed = findSectionsChanged (before, after);
    listeners.call ([&changed] (SongValues::Listener& listener) { listener.sectionsChanged (changed); });
}

std::vector<SectionID> findSectionsChanged (const SectionsValue& before, const SectionsValue& after)
{
    std::vector<SectionID> changed;

    for (const auto& section : after.sections)
        if (const auto* was = before.find (section.id); (was == nullptr || ! (*was == section)) && ! contains (changed, section.id))
            changed.push_back (section.id);

    for (const auto& section : before.sections)
        if (after.find (section.id) == nullptr && ! contains (changed, section.id))
            changed.push_back (section.id);

    return changed;
}

//==============================================================================
SongValues::SectionsKeeper::SectionsKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

void SongValues::SectionsKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    const auto isSections = child.hasType (schema::SECTIONS) && parent == state;

    if (! isSections && ! isHeardAsSections (parent, child))
        return;

    // The safety net (SongValues.h): a SECTIONS that comes into the song with sections in it has them read in as the song's
    // sections, and is emptied as adopt() empties the song's own (emptySectionsElement(): what Phazite doesn't know of it goes
    // too); a section added to the song's SECTIONS by itself is read into the sections and taken out again while the song is
    // written outside the history, and refused in a step, whose Undo, taking the tree's change back first, would find it gone
    // (JUCE workaround W62 (docs/juce-workarounds.md)). Either in the mode of the moment.
    auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

    if (isSections)
    {
        auto taken = 0;

        for (const auto& element : child)
            taken += element.hasType (schema::SECTION) ? 1 : 0;

        if (taken > 0)
        {
            values.sectionsAdopted += taken;
            values.setSections (song, readHeldSections (child));
        }

        // Emptied whether or not it held a section: what Phazite doesn't know of it goes too, so that it is the empty place.
        if (child.getNumChildren() > 0 || child.getNumProperties() > 0)
            emptySectionsElement (child, undo);

        return;
    }

    if (! child.hasType (schema::SECTION) || ! parent.hasType (schema::SECTIONS) || parent.getParent() != state)
        return;

    if (undo != nullptr)
    {
        jassertfalse; // a section added to the song's sections: the sections' functions change them (Sections.h)
        return;
    }

    ++values.sectionsAdopted;
    values.setSections (song, withSectionTaken (values.sections.get(), child));
    parent.removeChild (child, nullptr);
}

void SongValues::SectionsKeeper::adopt (SongValues&, Song& loaded)
{
    // The song's SECTIONS, whose sections its reader read into the area before (readHeldSectionsIn(): SongValues.cpp, adopt()),
    // emptied (emptySectionsElement()), and kept: it is the place a whole copy writes the sections in, written as the song had
    // it. Nothing for a song without one.
    if (auto element = loaded.getState().getChildWithName (schema::SECTIONS); element.isValid())
        emptySectionsElement (element, nullptr);
}

void readHeldSectionsIn (const juce::ValueTree& song, SongState& into)
{
    // The sections the loader made consistent, each as the song holds one, one of each ID (SectionsValue.h: readHeldSections()),
    // in the element's order; none for a song without a SECTIONS.
    if (const auto element = song.getChildWithName (schema::SECTIONS); element.isValid())
        into.sections = readHeldSections (element);
}

//==============================================================================
void SongValues::setSections (Song& song, SectionsValue next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next == sections.get())
        return;

    if (song.isWritingOutsideHistory())
        sections.setOutsideHistory (std::move (next));
    else
        sections.set (std::move (next));
}

bool isHeardAsSections (const juce::ValueTree& element)
{
    for (auto each = element; each.isValid(); each = each.getParent())
        if (each.hasType (schema::SECTIONS))
            return each.getParent().hasType (schema::PHAZITE);

    return false;
}

bool isHeardAsSections (const juce::ValueTree& parent, const juce::ValueTree&)
{
    return isHeardAsSections (parent);
}

//==============================================================================
bool leadsToSections (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::SECTIONS);
}

void writeSectionsIn (juce::ValueTree& element, const SongState& kept)
{
    // Nothing kept of no sections, nor of a song that keeps everything in its tree (whose SECTIONS holds its sections).
    if (! element.hasType (schema::SECTIONS) || kept.sections.sections.empty())
        return;

    jassert (element.getNumChildren() == 0); // the song's tree holds none of its sections (SongValues.h)
    writeSections (kept.sections, element);
}

bool holdSameSections (const SongState& kept, const SongState& otherKept)
{
    // Whether each has a SECTIONS among its root's places (an empty one is written into the file, and is no song without one),
    // and the values.
    if (kept.holdsPlace (RootPlace::sections) != otherKept.holdsPlace (RootPlace::sections))
        return false;

    return kept.sections.isSameAs (otherKept.sections) || kept.sections == otherKept.sections;
}

std::uint64_t findLargestIDIn (const SectionsValue& value)
{
    std::uint64_t largest = 0;

    for (const auto& section : value.sections)
        largest = countHeldID (largest, section.id.getRaw());

    return largest;
}
} // namespace phazite
