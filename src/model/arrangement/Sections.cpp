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

    juce::UndoManager* getUndo (te::Edit& edit)
    {
        return &getSongUndoManager (edit);
    }

    juce::ValueTree findSectionsState (const te::Edit& edit)
    {
        return findPhaziteState (edit).getChildWithName (schema::SECTIONS);
    }

    // The SECTIONS element, added if the song has none yet.
    juce::ValueTree getSectionsState (te::Edit& edit)
    {
        auto phazite = getPhaziteState (edit);
        auto sections = phazite.getChildWithName (schema::SECTIONS);

        if (! sections.isValid())
        {
            sections = juce::ValueTree (schema::SECTIONS);
            phazite.appendChild (sections, getUndo (edit));
        }

        return sections;
    }

    SectionID getID (const juce::ValueTree& state)
    {
        return SectionID::fromRaw (te::EditItemID::fromVar (state[schema::sectionID]).getRawID());
    }

    // A new ID of the song's, which no track, clip or other section of it has, even after an undo.
    SectionID giveNewID (te::Edit& edit, juce::ValueTree& state)
    {
        const auto id = SectionID::fromRaw (makeID (edit));
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
    std::vector<juce::ValueTree> getSectionStates (const te::Edit& edit)
    {
        std::vector<juce::ValueTree> states;

        for (const auto& state : findSectionsState (edit))
            if (state.hasType (schema::SECTION))
                states.push_back (state);

        std::stable_sort (states.begin(), states.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                          { return getRange (a).getStart() < getRange (b).getStart(); });
        return states;
    }

    juce::ValueTree findSectionState (const te::Edit& edit, SectionID id)
    {
        if (id.isValid())
            for (const auto& state : findSectionsState (edit))
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
    SectionID findSectionStartingAt (const te::Edit& edit, BeatPosition beat, SectionID except)
    {
        for (const auto& section : getSections (edit))
            if (section.id != except && std::abs ((section.range.getStart() - beat).inBeats()) < 1.0e-4)
                return section.id;

        return {};
    }

    bool contains (const std::vector<SectionID>& ids, SectionID id)
    {
        return std::find (ids.begin(), ids.end(), id) != ids.end();
    }

    // Where a bar starts, in beats, and the bar a beat is in (Tempo.h).
    BeatPosition getBarStart (const te::Edit& edit, int bar)
    {
        return getBars (edit).getBarStart (bar);
    }

    int getBar (const te::Edit& edit, BeatPosition beat)
    {
        return getBars (edit).getBarAt (beat);
    }
} // namespace

//==============================================================================
std::vector<Section> getSections (const te::Edit& edit)
{
    std::vector<Section> sections;

    for (const auto& state : getSectionStates (edit))
        sections.push_back (makeSection (state));

    return sections;
}

std::optional<Section> getSection (const te::Edit& edit, SectionID id)
{
    if (const auto state = findSectionState (edit, id); state.isValid())
        return makeSection (state);

    return std::nullopt;
}

std::optional<Section> getSectionAt (const te::Edit& edit, BeatPosition beat)
{
    for (const auto& section : getSections (edit))
        if (section.range.contains (beat))
            return section;

    return std::nullopt;
}

BeatPosition getSectionsEnd (const te::Edit& edit)
{
    BeatPosition end;

    for (const auto& state : findSectionsState (edit))
        if (state.hasType (schema::SECTION))
            end = std::max (end, getRange (state).getEnd());

    return end;
}

std::vector<Section> getSelectedSections (const Selection& selection)
{
    std::vector<Section> selected;

    if (auto* edit = selection.getEdit())
        for (const auto& section : getSections (*edit))
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

SectionID insertSection (te::Edit& edit, BeatPosition start, const SectionInfo& info)
{
    juce::ValueTree state (schema::SECTION);
    const auto id = giveNewID (edit, state);
    setRange (state, { start, start + info.length }, nullptr);
    state.setProperty (schema::sectionName, info.name, nullptr);

    if (info.role.isNotEmpty())
        state.setProperty (schema::sectionRole, info.role, nullptr);

    if (info.tip.isNotEmpty())
        state.setProperty (schema::sectionTip, info.tip, nullptr);

    state.setProperty (schema::sectionEnergy, juce::jlimit (0.0f, 1.0f, info.energy), nullptr);

    if (info.colour.has_value())
        state.setProperty (schema::sectionColour, info.colour->toString(), nullptr);

    getSectionsState (edit).appendChild (state, getUndo (edit));
    return id;
}

void removeSections (te::Edit& edit)
{
    auto sections = findSectionsState (edit);

    for (const auto& state : getSectionStates (edit))
        sections.removeChild (state, getUndo (edit));
}

juce::String getNewSectionName (const te::Edit& edit)
{
    return "Section " + juce::String (getSections (edit).size() + 1);
}

SectionID appendSection (te::Edit& edit, Selection* selection)
{
    const auto start = getSectionsEnd (edit);
    const auto end = getBarStart (edit, getBar (edit, start) + barsPerNewSection);
    const auto id = insertSection (edit, start, { getNewSectionName (edit), end - start });

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

SectionID addSectionAt (te::Edit& edit, BeatPosition beat, Selection* selection)
{
    // From the start of the bar (or the end of the section before, if that ends later), for barsPerNewSection bars or
    // up to the next section: sections never overlap, which the section edits rely on.
    const auto bar = std::max (0, getBar (edit, beat));
    auto start = getBarStart (edit, bar);
    auto end = getBarStart (edit, bar + barsPerNewSection);
    const auto sections = getSections (edit);

    for (const auto& other : sections)
        if (other.range.getStart() <= beat && other.range.getEnd() > start)
            start = other.range.getEnd();

    for (const auto& other : sections)
        if (other.range.getStart() > beat)
            end = std::min (end, other.range.getStart());

    if (end <= start)
        return {};

    const UndoStep step (edit, "Add Section");
    const auto id = insertSection (edit, start, { getNewSectionName (edit), end - start });

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

void renameSection (te::Edit& edit, SectionID id, const juce::String& name)
{
    auto state = findSectionState (edit, id);
    const auto trimmed = name.trim();

    if (! state.isValid() || trimmed.isEmpty() || trimmed == state[schema::sectionName].toString())
        return;

    const UndoStep step (edit, "Rename Section");
    state.setProperty (schema::sectionName, trimmed, getUndo (edit));
}

void setSectionColour (te::Edit& edit, SectionID id, juce::Colour colour)
{
    if (auto state = findSectionState (edit, id); state.isValid())
    {
        const UndoStep step (edit, "Change Section Color");
        state.setProperty (schema::sectionColour, colour.toString(), getUndo (edit));
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
void moveSection (te::Edit& edit, SectionID id, BeatPosition destination)
{
    if (const auto section = getSection (edit, id))
    {
        const UndoStep step (edit, "Move Section");
        moveTime (edit, toTracktion (section->range), toTracktion (destination));
    }
}

SectionID copySection (te::Edit& edit, SectionID id, BeatPosition destination, bool asAliases)
{
    const auto section = getSection (edit, id);

    if (! section.has_value())
        return {};

    const UndoStep step (edit, asAliases ? "Copy Section as Aliases" : "Copy Section");
    copyTime (edit, toTracktion (section->range), toTracktion (destination), asAliases);
    return findSectionStartingAt (edit, destination, id);
}

SectionID duplicateSection (te::Edit& edit, SectionID id, bool asAliases)
{
    const auto section = getSection (edit, id);
    return section.has_value() ? copySection (edit, id, section->range.getEnd(), asAliases) : SectionID();
}

void deleteSection (te::Edit& edit, SectionID id)
{
    if (const auto section = getSection (edit, id))
        removeTime (edit, toTracktion (section->range));
}

void deleteSections (te::Edit& edit, const std::vector<SectionID>& ids)
{
    const auto sections = getSections (edit);

    for (auto section = sections.rbegin(); section != sections.rend(); ++section)
        if (contains (ids, section->id))
            deleteSection (edit, section->id); // found again: one deleted with another is gone
}

std::vector<SectionID> duplicateSections (te::Edit& edit, const std::vector<SectionID>& ids, bool asAliases, Selection* selection)
{
    std::vector<SectionID> copies;

    if (ids.empty())
        return copies;

    if (selection != nullptr)
        selection->deselectAll();

    const auto sections = getSections (edit);

    for (auto section = sections.rbegin(); section != sections.rend(); ++section)
    {
        if (! contains (ids, section->id))
            continue;

        if (const auto copy = duplicateSection (edit, section->id, asAliases); copy.isValid())
        {
            copies.push_back (copy);

            if (selection != nullptr)
                selection->addToSelection (copy);
        }
    }

    return copies;
}

void resizeSection (te::Edit& edit, SectionID id, BeatDuration newLength, bool moveRest)
{
    auto state = findSectionState (edit, id);

    if (! state.isValid())
        return;

    const UndoStep step (edit, "Resize Section");
    const auto range = getRange (state);
    newLength = std::max (newLength, BeatDuration::fromBeats (1.0));

    if (std::abs ((newLength - range.getLength()).inBeats()) < tiny.inBeats())
        return;

    if (moveRest)
    {
        if (newLength > range.getLength())
        {
            insertTime (edit, toTracktion (range.getEnd()),
                        toTracktion (newLength - range.getLength())); // moves the sections after it, not it
            setRange (state, range.withEnd (range.getStart() + newLength), getUndo (edit));
        }
        else
        {
            removeTime (edit, toTracktion (BeatRange (range.getStart() + newLength, range.getEnd()))); // shortens it
        }

        return;
    }

    // The section alone, up to the next one at most.
    auto end = range.getStart() + newLength;

    for (const auto& other : getSections (edit))
        if (other.id != id && other.range.getStart() >= range.getEnd() - tiny)
            end = std::min (end, other.range.getStart());

    if (end > range.getStart() + tiny)
        setRange (state, range.withEnd (end), getUndo (edit));
}

std::vector<BeatPosition> getSectionBoundaries (const te::Edit& edit)
{
    std::vector<BeatPosition> boundaries;
    const auto sections = getSections (edit);

    for (const auto& section : sections)
        boundaries.push_back (section.range.getStart());

    if (! sections.empty())
        boundaries.push_back (sections.back().range.getEnd());

    return boundaries;
}

//==============================================================================
void insertSectionTime (te::Edit& edit, BeatPosition at, BeatDuration length)
{
    if (length <= BeatDuration())
        return;

    for (auto state : getSectionStates (edit))
    {
        const auto range = getRange (state);

        if (range.getStart() >= at - tiny)
            setRange (state, range.movedBy (length), getUndo (edit));
        else if (range.getEnd() > at + tiny)
            setRange (state, range.withEnd (range.getEnd() + length), getUndo (edit)); // the time is inside it
    }
}

void removeSectionTime (te::Edit& edit, BeatRange range)
{
    if (range.isEmpty())
        return;

    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();
    auto sections = findSectionsState (edit);

    for (auto state : getSectionStates (edit))
    {
        const auto r = getRange (state);

        if (r.getEnd() <= start + tiny)
            continue; // before the range

        if (r.getStart() >= end - tiny)
            setRange (state, r.movedBy (-length), getUndo (edit));
        else if (r.getStart() >= start - tiny && r.getEnd() <= end + tiny)
            sections.removeChild (state, getUndo (edit));
        else if (r.getStart() < start && r.getEnd() > end)
            setRange (state, r.withEnd (r.getEnd() - length), getUndo (edit));
        else if (r.getStart() < start)
            setRange (state, r.withEnd (start), getUndo (edit)); // its end was in the range
        else
            setRange (state, { start, r.getEnd() - length }, getUndo (edit)); // its start was in the range
    }
}

void moveSectionTime (te::Edit& edit, BeatRange range, BeatPosition destination)
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

    for (auto state : getSectionStates (edit))
    {
        const auto r = getRange (state);

        if (r.getStart() >= changedStart - tiny && r.getStart() < changedEnd - tiny)
        {
            const auto inRange = r.getStart() >= start - tiny && r.getStart() < end - tiny;
            setRange (state, r.movedBy (inRange ? rangeShift : otherShift), getUndo (edit));
        }
    }
}

SectionsCopy copySections (const te::Edit& edit, BeatRange range)
{
    SectionsCopy copy;
    copy.rangeStart = range.getStart();

    for (const auto& state : getSectionStates (edit))
        if (const auto r = getRange (state); r.getStart() >= range.getStart() - tiny && r.getEnd() <= range.getEnd() + tiny)
            copy.sections.push_back (state.createCopy());

    return copy;
}

void pasteSections (te::Edit& edit, const SectionsCopy& copy, BeatPosition destination)
{
    for (const auto& section : copy.sections)
    {
        auto pasted = section.createCopy();
        giveNewID (edit, pasted);
        setRange (pasted, getRange (section).movedBy (destination - copy.rangeStart), nullptr);
        getSectionsState (edit).appendChild (pasted, getUndo (edit));
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
                    .perform = withEdit (selection, [&selection] (te::Edit& edit) { appendSection (edit, &selection); }),
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget });
}
} // namespace phazite
