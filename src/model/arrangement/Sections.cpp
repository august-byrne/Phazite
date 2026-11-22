// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sections.h"
#include "internal/Arrangement.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace phazite
{
namespace
{
    const auto tiny = BeatDuration::fromBeats (1.0e-6);
    std::atomic<std::uint64_t> sectionListsBuilt { 0 }; // countSectionListsBuilt(): a count alone, which orders nothing

    // The song's SECTIONS, the place a whole copy writes its sections in (an empty placeholder: SectionsArea.h).
    juce::ValueTree findSectionsState (const Song& song)
    {
        return song.getState().getChildWithName (schema::SECTIONS);
    }

    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getTreeUndo (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // The song's SECTIONS added where it has none, after its other elements: the place of its sections, made with the first,
    // before the value that holds it (the value never says more than the element).
    void makeSectionsPlace (Song& song)
    {
        if (! findSectionsState (song).isValid())
            song.getState().appendChild (juce::ValueTree (schema::SECTIONS), getTreeUndo (song));
    }

    // The song's sections made the next value: their area's, in the step in progress, or outside the history while the song is
    // written outside it (SongValues.h: setSections(), which writes nothing for the sections the song holds already), kept as
    // the song holds them (SectionsValue.h: keepSectionsHeld(), the rule what enters the area keeps too).
    void setSections (Song& song, const SectionsValue& next)
    {
        if (auto* values = song.getValues())
            values->setSections (song, keepSectionsHeld (next));
        else
            jassertfalse; // the sections' edits are a song's that holds its values (model/project/internal/Project.h: makeSong())
    }

    // Where a section starts and ends, as its element read: from its start for its length (none for a length below 0).
    BeatRange getRange (const SectionValue& section)
    {
        return { section.start, section.start + section.length };
    }

    // A section given another range, as its element was: its start, and the range's length.
    SectionValue withRange (SectionValue section, BeatRange range)
    {
        section.start = range.getStart();
        section.length = range.getLength();
        return section;
    }

    // The place in the value of the section with an ID: the first with it; none for none, and for an invalid ID.
    std::optional<size_t> findPlace (const SectionsValue& value, SectionID id)
    {
        if (id.isValid())
            for (size_t i = 0; i < value.sections.size(); ++i)
                if (value.sections[i].id == id)
                    return i;

        return std::nullopt;
    }

    // The sections, each made the next one by an edit (the same, another, or none where it goes), in their order.
    template <typename Edit>
    SectionsValue editEach (const SectionsValue& value, Edit&& edit)
    {
        auto next = immer::flex_vector<SectionValue>().transient();

        for (const auto& section : value.sections)
            if (auto edited = edit (section))
                next.push_back (std::move (*edited));

        return { next.persistent() };
    }

    // How a merge names a section in its clash's path (SongMerge.cpp: keyOf()): its type and its key, its ID.
    juce::String nameInPath (const SectionValue& section)
    {
        return schema::SECTION.toString() + ":" + schema::getElementKey (writeSectionElement (section));
    }

    // A section of the song's value as the API gives it: its own colour, or else its role's.
    Section makeSection (const SectionValue& value)
    {
        Section section;
        section.id = value.id;
        section.range = { value.start, value.start + value.length };
        section.name = value.name;
        section.role = value.role;
        section.tip = value.tip;
        section.energy = value.energy;
        section.colour = value.colour.has_value() ? juce::Colour::fromString (*value.colour) : getRoleColour (section.role);
        return section;
    }

    // The section that starts at a beat, other than one to leave out.
    SectionID findSectionStartingAt (const Song& song, BeatPosition beat, SectionID except)
    {
        for (const auto& section : getSections (song))
            if (section.id != except && std::abs ((section.range.getStart() - beat).inBeats()) < 1.0e-4)
                return section.id;

        return {};
    }

    bool contains (const std::vector<SectionID>& ids, SectionID id)
    {
        return std::find (ids.begin(), ids.end(), id) != ids.end();
    }

    // Where a bar starts, in beats, and the bar a beat is in (Tempo.h).
    BeatPosition getBarStart (const Song& song, int bar)
    {
        return getBars (song).getBarStart (bar);
    }

    int getBar (const Song& song, BeatPosition beat)
    {
        return getBars (song).getBarAt (beat);
    }
} // namespace

//==============================================================================
std::vector<Section> getSections (const Song& song)
{
    // In the song's order: by their starts, two at one start in the order the song holds them.
    const auto value = getSectionsValue (song);
    std::vector<Section> sections;
    sections.reserve (value.sections.size());
    sectionListsBuilt.fetch_add (1, std::memory_order_relaxed);

    for (const auto& section : value.sections)
        sections.push_back (makeSection (section));

    std::stable_sort (sections.begin(), sections.end(),
                      [] (const Section& a, const Section& b) { return a.range.getStart() < b.range.getStart(); });
    return sections;
}

std::uint64_t countSectionListsBuilt() noexcept
{
    return sectionListsBuilt.load (std::memory_order_relaxed);
}

std::optional<Section> getSection (const Song& song, SectionID id)
{
    if (! id.isValid())
        return std::nullopt;

    const auto value = getSectionsValue (song);

    if (const auto* section = value.find (id))
        return makeSection (*section);

    return std::nullopt;
}

std::optional<Section> getSectionAt (const Song& song, BeatPosition beat)
{
    for (const auto& section : getSections (song))
        if (section.range.contains (beat))
            return section;

    return std::nullopt;
}

BeatPosition getSectionsEnd (const Song& song)
{
    const auto value = getSectionsValue (song);
    BeatPosition end;

    for (const auto& section : value.sections)
        end = std::max (end, section.start + section.length);

    return end;
}

std::vector<Section> getSelectedSections (const Selection& selection)
{
    std::vector<Section> selected;
    const auto& ids = selection.getSections();

    // The song's sections read once: each is there, so a selected one is one the selection holds (Selection::isSelected()).
    if (auto* song = selection.getSong(); song != nullptr && ! ids.empty())
        for (const auto& section : getSections (*song))
            if (contains (ids, section.id))
                selected.push_back (section);

    return selected;
}

bool hasSelectedSections (const Selection& selection)
{
    return ! getSelectedSections (selection).empty();
}

std::vector<SectionID> getIDs (const std::vector<Section>& sections)
{
    std::vector<SectionID> ids;

    for (const auto& section : sections)
        ids.push_back (section.id);

    return ids;
}

juce::Colour getRoleColour (const juce::String& role)
{
    struct RoleColour
    {
        const char* role;
        const char* colour;
    };

    static constexpr RoleColour roleColours[] = { { "intro", "Gray" },
                                                  { "outro", "Gray" },
                                                  { "verse", "Blue" },
                                                  { "preChorus", "Yellow" },
                                                  { "buildUp", "Yellow" },
                                                  { "chorus", "Vermilion" },
                                                  { "hook", "Vermilion" },
                                                  { "drop", "Vermilion" },
                                                  { "bridge", "Reddish purple" },
                                                  { "breakdown", "Reddish purple" },
                                                  { "solo", "Bluish green" } };

    const auto skyBlue = getTrackColours()[1].colour;

    for (const auto& entry : roleColours)
        if (role == entry.role)
            return findTrackColour (entry.colour).value_or (skyBlue);

    return skyBlue;
}

SectionID insertSection (Song& song, BeatPosition start, const SectionInfo& info)
{
    // A new ID of the song's, which no track, clip or other section of it has, even after an undo; after the others.
    SectionValue section;
    section.id = SectionID::fromRaw (song.makeID());
    section = withRange (section, { start, start + info.length });
    section.name = info.name;
    section.role = info.role; // none written where it is empty, nor a tip (SectionsValue.h: writeSectionElement())
    section.tip = info.tip;
    section.energy = juce::jlimit (0.0f, 1.0f, info.energy);

    if (info.colour.has_value())
        section.colour = info.colour->toString();

    makeSectionsPlace (song);
    setSections (song, { getSectionsValue (song).sections.push_back (section) });
    return section.id;
}

void removeSections (Song& song)
{
    // Its SECTIONS stays, empty, as it was.
    setSections (song, {});
}

juce::String getNewSectionName (const Song& song)
{
    return "Section " + juce::String (getSections (song).size() + 1);
}

SectionID appendSection (Song& song, Selection* selection)
{
    const auto start = getSectionsEnd (song);
    const auto end = getBarStart (song, getBar (song, start) + barsPerNewSection);
    const auto id = insertSection (song, start, { getNewSectionName (song), end - start });

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

SectionID addSectionAt (Song& song, BeatPosition beat, Selection* selection)
{
    // From the start of the bar (or the end of the section before, if that ends later), for barsPerNewSection bars or
    // up to the next section: sections never overlap, which the section edits rely on.
    const auto bar = std::max (0, getBar (song, beat));
    auto start = getBarStart (song, bar);
    auto end = getBarStart (song, bar + barsPerNewSection);
    const auto sections = getSections (song);

    for (const auto& other : sections)
        if (other.range.getStart() <= beat && other.range.getEnd() > start)
            start = other.range.getEnd();

    for (const auto& other : sections)
        if (other.range.getStart() > beat)
            end = std::min (end, other.range.getStart());

    if (end <= start)
        return {};

    const UndoStep step (song, "Add Section");
    const auto id = insertSection (song, start, { getNewSectionName (song), end - start });

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

void renameSection (Song& song, SectionID id, const juce::String& name)
{
    const auto value = getSectionsValue (song);
    const auto place = findPlace (value, id);
    const auto trimmed = name.trim();

    if (! place.has_value() || trimmed.isEmpty() || trimmed == value.sections[*place].name)
        return;

    const UndoStep step (song, "Rename Section");
    auto section = value.sections[*place];
    section.name = trimmed;
    setSections (song, { value.sections.set (*place, std::move (section)) });
}

void setSectionColour (Song& song, SectionID id, juce::Colour colour)
{
    const auto value = getSectionsValue (song);

    if (const auto place = findPlace (value, id))
    {
        const UndoStep step (song, "Change Section Color");
        auto section = value.sections[*place];
        section.colour = colour.toString();
        setSections (song, { value.sections.set (*place, std::move (section)) });
    }
}

BeatRange getSectionsRange (const std::vector<Section>& sections)
{
    if (sections.empty())
        return {};

    auto range = sections.front().range;

    for (const auto& section : sections)
        range = range.getUnionWith (section.range);

    return range;
}

//==============================================================================
void moveSection (Song& song, SectionID id, BeatPosition destination)
{
    if (const auto section = getSection (song, id))
    {
        const UndoStep step (song, "Move Section");
        moveTime (song, section->range, destination);
    }
}

SectionID copySection (Song& song, SectionID id, BeatPosition destination, bool asAliases)
{
    const auto section = getSection (song, id);

    if (! section.has_value())
        return {};

    const UndoStep step (song, asAliases ? "Copy Section as Aliases" : "Copy Section");
    copyTime (song, section->range, destination, asAliases);
    return findSectionStartingAt (song, destination, id);
}

SectionID duplicateSection (Song& song, SectionID id, bool asAliases)
{
    const auto section = getSection (song, id);
    return section.has_value() ? copySection (song, id, section->range.getEnd(), asAliases) : SectionID();
}

void deleteSection (Song& song, SectionID id)
{
    if (const auto section = getSection (song, id))
        removeTime (song, section->range);
}

void deleteSections (Song& song, const std::vector<SectionID>& ids)
{
    const auto sections = getSections (song);

    for (auto section = sections.rbegin(); section != sections.rend(); ++section)
        if (contains (ids, section->id))
            deleteSection (song, section->id); // found again: one deleted with another is gone
}

std::vector<SectionID> duplicateSections (Song& song, const std::vector<SectionID>& ids, bool asAliases, Selection* selection)
{
    std::vector<SectionID> copies;

    if (ids.empty())
        return copies;

    if (selection != nullptr)
        selection->deselectAll();

    const auto sections = getSections (song);

    for (auto section = sections.rbegin(); section != sections.rend(); ++section)
    {
        if (! contains (ids, section->id))
            continue;

        if (const auto copy = duplicateSection (song, section->id, asAliases); copy.isValid())
        {
            copies.push_back (copy);

            if (selection != nullptr)
                selection->addToSelection (copy);
        }
    }

    return copies;
}

void resizeSection (Song& song, SectionID id, BeatDuration newLength, bool moveRest)
{
    const auto before = getSectionsValue (song);
    const auto place = findPlace (before, id);

    if (! place.has_value())
        return;

    const UndoStep step (song, "Resize Section");
    const auto range = getRange (before.sections[*place]);
    newLength = std::max (newLength, BeatDuration::fromBeats (1.0));

    if (std::abs ((newLength - range.getLength()).inBeats()) < tiny.inBeats())
        return;

    // The section given a range in the sections as they are now, found again by its ID (an edit of time moved others).
    const auto setRange = [&song, id] (BeatRange newRange)
    {
        const auto value = getSectionsValue (song);

        if (const auto at = findPlace (value, id))
            setSections (song, { value.sections.set (*at, withRange (value.sections[*at], newRange)) });
    };

    if (moveRest)
    {
        if (newLength > range.getLength())
        {
            insertTime (song, range.getEnd(), newLength - range.getLength()); // moves the sections after it, not it
            setRange (range.withEnd (range.getStart() + newLength));
        }
        else
        {
            removeTime (song, BeatRange (range.getStart() + newLength, range.getEnd())); // shortens it
        }

        return;
    }

    // The section alone, up to the next one at most.
    auto end = range.getStart() + newLength;

    for (const auto& other : getSections (song))
        if (other.id != id && other.range.getStart() >= range.getEnd() - tiny)
            end = std::min (end, other.range.getStart());

    if (end > range.getStart() + tiny)
        setRange (range.withEnd (end));
}

std::vector<BeatPosition> getSectionBoundaries (const Song& song)
{
    std::vector<BeatPosition> boundaries;
    const auto sections = getSections (song);

    for (const auto& section : sections)
        boundaries.push_back (section.range.getStart());

    if (! sections.empty())
        boundaries.push_back (sections.back().range.getEnd());

    return boundaries;
}

//==============================================================================
void insertSectionTime (Song& song, BeatPosition at, BeatDuration length)
{
    if (length <= BeatDuration())
        return;

    // Each section on its own (none reads another's), the next value set once.
    setSections (song, editEach (getSectionsValue (song),
                                 [&] (const SectionValue& section) -> std::optional<SectionValue>
                                 {
                                     const auto range = getRange (section);

                                     if (range.getStart() >= at - tiny)
                                         return withRange (section, range.movedBy (length));

                                     if (range.getEnd() > at + tiny)
                                         return withRange (section, range.withEnd (range.getEnd() + length)); // the time is inside it

                                     return section;
                                 }));
}

void removeSectionTime (Song& song, BeatRange range)
{
    if (range.isEmpty())
        return;

    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();

    setSections (song, editEach (getSectionsValue (song),
                                 [&] (const SectionValue& section) -> std::optional<SectionValue>
                                 {
                                     const auto r = getRange (section);

                                     if (r.getEnd() <= start + tiny)
                                         return section; // before the range

                                     if (r.getStart() >= end - tiny)
                                         return withRange (section, r.movedBy (-length));

                                     if (r.getStart() >= start - tiny && r.getEnd() <= end + tiny)
                                         return std::nullopt; // inside it: it goes

                                     if (r.getStart() < start && r.getEnd() > end)
                                         return withRange (section, r.withEnd (r.getEnd() - length));

                                     if (r.getStart() < start)
                                         return withRange (section, r.withEnd (start)); // its end was in the range

                                     return withRange (section, { start, r.getEnd() - length }); // its start was in the range
                                 }));
}

void moveSectionTime (Song& song, BeatRange range, BeatPosition destination)
{
    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();
    const auto later = destination >= end - tiny;

    if (range.isEmpty() || (destination > start + tiny && destination < end - tiny))
        return;

    const auto changedStart = later ? start : destination;
    const auto changedEnd = later ? destination : end;
    const auto rangeShift = later ? destination - end : destination - start;
    const auto otherShift = later ? -length : length;

    setSections (song, editEach (getSectionsValue (song),
                                 [&] (const SectionValue& section) -> std::optional<SectionValue>
                                 {
                                     const auto r = getRange (section);

                                     if (r.getStart() >= changedStart - tiny && r.getStart() < changedEnd - tiny)
                                     {
                                         const auto inRange = r.getStart() >= start - tiny && r.getStart() < end - tiny;
                                         return withRange (section, r.movedBy (inRange ? rangeShift : otherShift));
                                     }

                                     return section;
                                 }));
}

SectionsCopy copySections (const Song& song, BeatRange range)
{
    SectionsCopy copy;
    copy.rangeStart = range.getStart();

    for (const auto& section : getSectionsValue (song).sections)
        if (const auto r = getRange (section); r.getStart() >= range.getStart() - tiny && r.getEnd() <= range.getEnd() + tiny)
            copy.sections.push_back (section);

    // In song order: by their starts, two at one start in the order the song holds them.
    std::stable_sort (copy.sections.begin(), copy.sections.end(),
                      [] (const SectionValue& a, const SectionValue& b) { return a.start < b.start; });
    return copy;
}

void pasteSections (Song& song, const SectionsCopy& copy, BeatPosition destination)
{
    if (copy.sections.empty())
        return;

    // Each with a new ID of the song's, in the copy's order, after the others; the song's SECTIONS first where it has none.
    auto sections = getSectionsValue (song).sections;

    for (const auto& section : copy.sections)
    {
        auto pasted = section;
        pasted.id = SectionID::fromRaw (song.makeID());
        sections = sections.push_back (withRange (std::move (pasted), getRange (section).movedBy (destination - copy.rangeStart)));
    }

    makeSectionsPlace (song);
    setSections (song, { sections });
}

bool makeSectionsAs (Song& song, const juce::ValueTree& sections)
{
    auto* values = song.getValues();

    if (values == nullptr || (sections.isValid() && ! sections.hasType (schema::SECTIONS)))
        return false;

    // Its element first where it comes, its value, then its element away where it goes: the value never says more than the
    // element does (a song without a SECTIONS holds no sections).
    auto state = song.getState();
    const auto live = findSectionsState (song);
    const auto before = values->sections.get();
    auto wrote = false;

    if (sections.isValid() && ! live.isValid())
    {
        state.appendChild (juce::ValueTree (schema::SECTIONS), getTreeUndo (song));
        wrote = true;
    }

    values->setSections (song, sections.isValid() ? readHeldSections (sections) : SectionsValue {});
    wrote = wrote || ! values->sections.get().isSameAs (before);

    if (! sections.isValid() && live.isValid())
    {
        state.removeChild (live, getTreeUndo (song));
        wrote = true;
    }

    return wrote;
}

AreaChoice chooseSectionClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A path into the song's SECTIONS, whose sections the song holds as a value beside its tree (model/base/internal/
    // SongValues.h), its tree's element an empty placeholder: a section merges by its ID, a property at a time (SongMerge.cpp),
    // so its clash is one section's property, or the whole section's (added or taken out on a side), by its ID, the path's
    // second step. A property: the value chosen given to the section of that ID (SectionsValue.h: setSectionProperty(), a void
    // value taking it away as the tree did), refused where the song holds no section of that ID, for the ID itself (the path's
    // key), for a property that isn't a section's, and where the section would not hold what was chosen (one the song reads
    // otherwise: SectionsValue.h, readHeldSection(): a length of 0 or less, a number that is none or beyond its range). A whole
    // section: the side chosen's section put in the place of the song's section of that ID, or after the others where it has
    // none; or the song's taken away where that side had none: as the song's tree had it chosen, in the choice's step. Refused
    // (nothing written) for any other clash on its path, which a merge of two songs Phazite holds never makes, but a commit's
    // song may come from anyone (tests/FuzzTests.cpp chooses every clash of a damaged merge): the SECTIONS itself, an element
    // of another type, a section the song can't hold or of another ID, one that stands in for what its version couldn't read,
    // and a song without a SECTIONS (where the tree had no place to choose in).
    if (clash.path.isEmpty() || ! clash.path[0].startsWith (schema::SECTIONS.toString() + ":"))
        return AreaChoice::notTheArea;

    if (clash.path.size() != 2 || ! clash.path[1].startsWith (schema::SECTION.toString() + ":") || ! juce::isPositiveAndBelow (value, 3)
        || song.getValues() == nullptr || ! findSectionsState (song).isValid())
        return AreaChoice::refused;

    const auto sections = getSectionsValue (song);
    std::optional<size_t> place;

    for (size_t i = 0; i < sections.sections.size() && ! place.has_value(); ++i)
        if (nameInPath (sections.sections[i]) == clash.path[1])
            place = i;

    if (clash.property.isValid())
    {
        const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };

        if (! place.has_value() || clash.property == schema::sectionID)
            return AreaChoice::refused;

        auto section = sections.sections[*place];

        if (! setSectionProperty (section, clash.property, *values[value]) || readHeldSection (writeSectionElement (section)) != section)
            return AreaChoice::refused;

        setSections (song, { sections.sections.set (*place, std::move (section)) });
        return AreaChoice::chosen;
    }

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    const auto& chosen = *elements[value];
    const auto section = chosen.isValid() ? readHeldSection (chosen) : std::nullopt;

    if (chosen.isValid() && (! section.has_value() || holdsStandIn (chosen) || nameInPath (*section) != clash.path[1]))
        return AreaChoice::refused;

    const auto at = place.value_or (sections.sections.size());
    auto next = place.has_value() ? sections.sections.erase (*place) : sections.sections;

    if (section.has_value())
        next = next.insert (juce::jmin (at, next.size()), *section);

    setSections (song, { next });
    return AreaChoice::chosen;
}

//==============================================================================
void addSectionCommands (CommandRegistry& registry, Selection& selection)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };

    registry.add ({ .id = "section.add",
                    .name = "Add Section",
                    .category = "Section",
                    .description = "Adds an 8-bar section after the last one, or at the start of the song if it has none.",
                    .synonyms = { "new section", "part", "arranger", "marker" },
                    .perform = withSong (selection, [&selection] (Song& song) { appendSection (song, &selection); }),
                    .isEnabled = hasSong,
                    .undoTarget = undoTarget });
}
} // namespace phazite
