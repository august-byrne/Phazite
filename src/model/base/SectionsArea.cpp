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
void readHeldSectionsIn (const juce::ValueTree& song, SongState& into)
{
    // The sections the loader made consistent, each as the song holds one, one of each ID (SectionsValue.h: readHeldSections()),
    // in the element's order; none for a song without a SECTIONS, or with an empty one (an empty list is none).
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

//==============================================================================
bool leadsToSections (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::SECTIONS);
}

void writeSectionsIn (juce::ValueTree& element, const SongState& kept)
{
    // Nothing kept of no sections, whose song has no SECTIONS (SongState.h).
    if (! element.hasType (schema::SECTIONS) || kept.sections.sections.empty())
        return;

    writeSections (kept.sections, element);
}

bool holdSameSections (const SongState& kept, const SongState& otherKept)
{
    // The values alone: none is none, an empty SECTIONS being written nowhere (SongState.h).
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
