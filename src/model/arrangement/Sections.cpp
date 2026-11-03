// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sections.h"
#include "internal/Arrangement.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    const auto tiny = BeatDuration::fromBeats (1.0e-6);

    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    juce::ValueTree findSectionsState (const Song& song)
    {
        return song.getState().getChildWithName (schema::SECTIONS);
    }

    // The SECTIONS element, added if the song has none yet.
    juce::ValueTree getSectionsState (Song& song)
    {
        auto phazite = song.getState();
        auto sections = phazite.getChildWithName (schema::SECTIONS);

        if (! sections.isValid())
        {
            sections = juce::ValueTree (schema::SECTIONS);
            phazite.appendChild (sections, getUndo (song));
        }

        return sections;
    }

    SectionID getID (const juce::ValueTree& state)
    {
        return SectionID::fromRaw ((std::uint64_t) (juce::int64) state[schema::sectionID]);
    }

    // A new ID of the song's, which no track, clip or other section of it has, even after an undo.
    SectionID giveNewID (Song& song, juce::ValueTree& state)
    {
        const auto id = SectionID::fromRaw (song.makeID());
        state.setProperty (schema::sectionID, (juce::int64) id.getRaw(), nullptr);
        return id;
    }

    BeatRange getRange (const juce::ValueTree& state)
    {
        const auto start = BeatPosition::fromBeats (state[schema::sectionStart]);
        return { start, start + BeatDuration::fromBeats (state[schema::sectionLength]) };
    }

    void setRange (juce::ValueTree& state, BeatRange range, juce::UndoManager* undo)
    {
        state.setProperty (schema::sectionStart, range.getStart().inBeats(), undo);
        state.setProperty (schema::sectionLength, range.getLength().inBeats(), undo);
    }

    // The sections' elements, in song order.
    std::vector<juce::ValueTree> getSectionStates (const Song& song)
    {
        std::vector<juce::ValueTree> states;

        for (const auto& state : findSectionsState (song))
            if (state.hasType (schema::SECTION))
                states.push_back (state);

        std::stable_sort (states.begin(), states.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                          { return getRange (a).getStart() < getRange (b).getStart(); });
        return states;
    }

    juce::ValueTree findSectionState (const Song& song, SectionID id)
    {
        if (id.isValid())
            for (const auto& state : findSectionsState (song))
                if (state.hasType (schema::SECTION) && getID (state) == id)
                    return state;

        return {};
    }

    Section makeSection (const juce::ValueTree& state)
    {
        Section section;
        section.id = getID (state);
        section.range = getRange (state);
        section.name = state[schema::sectionName].toString();
        section.role = state[schema::sectionRole].toString();
        section.tip = state[schema::sectionTip].toString();
        section.energy = juce::jlimit (0.0f, 1.0f, (float) static_cast<double> (state.getProperty (schema::sectionEnergy, 0.5)));
        section.colour = state.hasProperty (schema::sectionColour) ? juce::Colour::fromString (state[schema::sectionColour].toString())
                                                                   : getRoleColour (section.role);
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
    std::vector<Section> sections;

    for (const auto& state : getSectionStates (song))
        sections.push_back (makeSection (state));

    return sections;
}

std::optional<Section> getSection (const Song& song, SectionID id)
{
    if (const auto state = findSectionState (song, id); state.isValid())
        return makeSection (state);

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
    BeatPosition end;

    for (const auto& state : findSectionsState (song))
        if (state.hasType (schema::SECTION))
            end = std::max (end, getRange (state).getEnd());

    return end;
}

std::vector<Section> getSelectedSections (const Selection& selection)
{
    std::vector<Section> selected;

    if (auto* song = selection.getSong())
        for (const auto& section : getSections (*song))
            if (selection.isSelected (section.id))
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
    juce::ValueTree state (schema::SECTION);
    const auto id = giveNewID (song, state);
    setRange (state, { start, start + info.length }, nullptr);
    state.setProperty (schema::sectionName, info.name, nullptr);

    if (info.role.isNotEmpty())
        state.setProperty (schema::sectionRole, info.role, nullptr);

    if (info.tip.isNotEmpty())
        state.setProperty (schema::sectionTip, info.tip, nullptr);

    state.setProperty (schema::sectionEnergy, juce::jlimit (0.0f, 1.0f, info.energy), nullptr);

    if (info.colour.has_value())
        state.setProperty (schema::sectionColour, info.colour->toString(), nullptr);

    getSectionsState (song).appendChild (state, getUndo (song));
    return id;
}

void removeSections (Song& song)
{
    auto sections = findSectionsState (song);

    for (const auto& state : getSectionStates (song))
        sections.removeChild (state, getUndo (song));
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
    auto state = findSectionState (song, id);
    const auto trimmed = name.trim();

    if (! state.isValid() || trimmed.isEmpty() || trimmed == state[schema::sectionName].toString())
        return;

    const UndoStep step (song, "Rename Section");
    state.setProperty (schema::sectionName, trimmed, getUndo (song));
}

void setSectionColour (Song& song, SectionID id, juce::Colour colour)
{
    if (auto state = findSectionState (song, id); state.isValid())
    {
        const UndoStep step (song, "Change Section Color");
        state.setProperty (schema::sectionColour, colour.toString(), getUndo (song));
    }
}

BeatRange getSectionsRange (const std::vector<Section>& sections)
{
    if (sections.empty())
        return {};

    auto range = sections.front().range;

    for (const auto& section : sections)
        range = { std::min (range.getStart(), section.range.getStart()), std::max (range.getEnd(), section.range.getEnd()) };

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
    auto state = findSectionState (song, id);

    if (! state.isValid())
        return;

    const UndoStep step (song, "Resize Section");
    const auto range = getRange (state);
    newLength = std::max (newLength, BeatDuration::fromBeats (1.0));

    if (std::abs ((newLength - range.getLength()).inBeats()) < tiny.inBeats())
        return;

    if (moveRest)
    {
        if (newLength > range.getLength())
        {
            insertTime (song, range.getEnd(), newLength - range.getLength()); // moves the sections after it, not it
            setRange (state, range.withEnd (range.getStart() + newLength), getUndo (song));
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
        setRange (state, range.withEnd (end), getUndo (song));
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

    for (auto state : getSectionStates (song))
    {
        const auto range = getRange (state);

        if (range.getStart() >= at - tiny)
            setRange (state, range.movedBy (length), getUndo (song));
        else if (range.getEnd() > at + tiny)
            setRange (state, range.withEnd (range.getEnd() + length), getUndo (song)); // the time is inside it
    }
}

void removeSectionTime (Song& song, BeatRange range)
{
    if (range.isEmpty())
        return;

    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();
    auto sections = findSectionsState (song);

    for (auto state : getSectionStates (song))
    {
        const auto r = getRange (state);

        if (r.getEnd() <= start + tiny)
            continue; // before the range

        if (r.getStart() >= end - tiny)
            setRange (state, r.movedBy (-length), getUndo (song));
        else if (r.getStart() >= start - tiny && r.getEnd() <= end + tiny)
            sections.removeChild (state, getUndo (song));
        else if (r.getStart() < start && r.getEnd() > end)
            setRange (state, r.withEnd (r.getEnd() - length), getUndo (song));
        else if (r.getStart() < start)
            setRange (state, r.withEnd (start), getUndo (song)); // its end was in the range
        else
            setRange (state, { start, r.getEnd() - length }, getUndo (song)); // its start was in the range
    }
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

    for (auto state : getSectionStates (song))
    {
        const auto r = getRange (state);

        if (r.getStart() >= changedStart - tiny && r.getStart() < changedEnd - tiny)
        {
            const auto inRange = r.getStart() >= start - tiny && r.getStart() < end - tiny;
            setRange (state, r.movedBy (inRange ? rangeShift : otherShift), getUndo (song));
        }
    }
}

SectionsCopy copySections (const Song& song, BeatRange range)
{
    SectionsCopy copy;
    copy.rangeStart = range.getStart();

    for (const auto& state : getSectionStates (song))
        if (const auto r = getRange (state); r.getStart() >= range.getStart() - tiny && r.getEnd() <= range.getEnd() + tiny)
            copy.sections.push_back (state.createCopy());

    return copy;
}

void pasteSections (Song& song, const SectionsCopy& copy, BeatPosition destination)
{
    for (const auto& section : copy.sections)
    {
        auto pasted = section.createCopy();
        giveNewID (song, pasted);
        setRange (pasted, getRange (section).movedBy (destination - copy.rangeStart), nullptr);
        getSectionsState (song).appendChild (pasted, getUndo (song));
    }
}

//==============================================================================
void addSectionCommands (CommandRegistry& registry, Selection& selection)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "section.add",
                    .name = "Add Section",
                    .category = "Section",
                    .description = "Adds an 8-bar section after the last one, or at the start of the song if it has none.",
                    .synonyms = { "new section", "part", "arranger", "marker" },
                    .perform = withSong (selection, [&selection] (Song& song) { appendSection (song, &selection); }),
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget });
}
} // namespace phazite
