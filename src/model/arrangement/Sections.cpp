// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Sections.h"
#include "Arrangement.h"
#include "Tracks.h"
#include "model/base/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/Undo.h"

#include <algorithm>
#include <vector>

namespace phazite
{
namespace
{
    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);

    te::BeatRange getRange (const te::Clip& section)
    {
        return { section.getStartBeat(), section.getEndBeat() };
    }

    // The section that starts at a beat, other than one to leave out.
    te::ArrangerClip* findSectionStartingAt (const te::Edit& edit, te::BeatPosition beat, const te::Clip* except)
    {
        for (auto* section : getSections (edit))
            if (section != except && std::abs ((section->getStartBeat() - beat).inBeats()) < 1.0e-4)
                return section;

        return nullptr;
    }

    // Sections, the last first, held so that one deleted by an edit of another is skipped.
    std::vector<te::SafeSelectable<te::ArrangerClip>> toSafeLatestFirst (const juce::Array<te::ArrangerClip*>& sections)
    {
        std::vector<te::SafeSelectable<te::ArrangerClip>> safe;

        for (int i = sections.size(); --i >= 0;)
            safe.emplace_back (sections[i]);

        return safe;
    }
} // namespace

//==============================================================================
juce::Array<te::ArrangerClip*> getSections (const te::Edit& edit)
{
    juce::Array<te::ArrangerClip*> sections;

    if (auto* arranger = edit.getArrangerTrack())
        for (auto* clip : arranger->getClips())
            if (auto* section = dynamic_cast<te::ArrangerClip*> (clip))
                sections.add (section);

    std::stable_sort (sections.begin(), sections.end(),
                      [] (te::ArrangerClip* a, te::ArrangerClip* b) { return a->getPosition().getStart() < b->getPosition().getStart(); });
    return sections;
}

te::ArrangerClip* getSectionAt (const te::Edit& edit, te::BeatPosition beat)
{
    for (auto* section : getSections (edit))
        if (beat >= section->getStartBeat() && beat < section->getEndBeat())
            return section;

    return nullptr;
}

juce::Array<te::ArrangerClip*> getSelectedSections (const te::SelectionManager& selection)
{
    juce::Array<te::ArrangerClip*> selected;

    if (auto* edit = selection.getEdit())
        for (auto* section : getSections (*edit))
            if (selection.isSelected (section))
                selected.add (section);

    return selected;
}

bool hasSelectedSections (const te::SelectionManager& selection)
{
    return ! getSelectedSections (selection).isEmpty();
}

juce::String getSectionRole (const te::Clip& section)
{
    return section.state[schema::phaziteRole].toString();
}

juce::String getSectionTip (const te::Clip& section)
{
    return section.state[schema::phaziteTip].toString();
}

float getSectionEnergy (const te::Clip& section)
{
    return juce::jlimit (0.0f, 1.0f, (float) static_cast<double> (section.state.getProperty (schema::phaziteEnergy, 0.5)));
}

juce::Colour getSectionColour (const te::Clip& section)
{
    return section.state.hasProperty (te::IDs::colour) ? section.getColour() : getRoleColour (getSectionRole (section));
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

te::ArrangerClip* insertSection (te::Edit& edit, te::BeatPosition start, const SectionInfo& info)
{
    edit.ensureArrangerTrack();
    auto* arranger = edit.getArrangerTrack();

    if (arranger == nullptr)
        return nullptr;

    const auto time = edit.tempoSequence.toTime (te::BeatRange (start, start + info.length));
    auto* section = dynamic_cast<te::ArrangerClip*> (arranger->insertNewClip (te::TrackItem::Type::arranger, info.name, time, nullptr));

    if (section == nullptr)
        return nullptr;

    auto* undoManager = &edit.getUndoManager();
    section->setColour (info.colour.value_or (getRoleColour (info.role)));
    section->state.setProperty (schema::phaziteRole, info.role, undoManager);
    section->state.setProperty (schema::phaziteTip, info.tip, undoManager);
    section->state.setProperty (schema::phaziteEnergy, juce::jlimit (0.0f, 1.0f, info.energy), undoManager);
    return section;
}

juce::String getNewSectionName (const te::Edit& edit)
{
    return "Section " + juce::String (getSections (edit).size() + 1);
}

te::ArrangerClip* appendSection (te::Edit& edit, te::SelectionManager* selection)
{
    const auto sections = getSections (edit);
    const auto start = sections.isEmpty() ? te::BeatPosition() : sections.getLast()->getEndBeat();
    const auto& tempo = edit.tempoSequence;
    const auto bar = tempo.toBarsAndBeats (tempo.toTime (start)).bars;
    const auto end = tempo.toBeats (te::tempo::BarsAndBeats { bar + barsPerNewSection, {}, 0 });
    auto* section = insertSection (edit, start, { getNewSectionName (edit), end - start });

    if (section != nullptr && selection != nullptr)
        selection->selectOnly (section);

    return section;
}

te::ArrangerClip* addSectionAt (te::Edit& edit, te::BeatPosition beat, te::SelectionManager* selection)
{
    // From the start of the bar (or the end of the section before, if that ends later), for barsPerNewSection bars or
    // up to the next section: sections never overlap, which the section edits rely on.
    const auto& tempo = edit.tempoSequence;
    const auto bar = std::max (0, tempo.toBarsAndBeats (tempo.toTime (beat)).bars);
    auto start = tempo.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 });
    auto end = tempo.toBeats (te::tempo::BarsAndBeats { bar + barsPerNewSection, {}, 0 });
    const auto sections = getSections (edit);

    for (auto* other : sections)
        if (other->getStartBeat() <= beat && other->getEndBeat() > start)
            start = other->getEndBeat();

    for (auto* other : sections)
        if (other->getStartBeat() > beat)
            end = std::min (end, other->getStartBeat());

    if (end <= start)
        return nullptr;

    const UndoStep step (edit, "Add Section");
    auto* section = insertSection (edit, start, { getNewSectionName (edit), end - start });

    if (section != nullptr && selection != nullptr)
        selection->selectOnly (section);

    return section;
}

void renameSection (te::ArrangerClip& section, const juce::String& name)
{
    const auto trimmed = name.trim();

    if (trimmed.isEmpty() || trimmed == section.getName())
        return;

    const UndoStep step (section.edit, "Rename Section");
    section.setName (trimmed);
}

void setSectionColour (te::ArrangerClip& section, juce::Colour colour)
{
    const UndoStep step (section.edit, "Change Section Color");
    section.setColour (colour);
}

te::TimeRange getSectionsTimeRange (const juce::Array<te::ArrangerClip*>& sections)
{
    if (sections.isEmpty())
        return {};

    auto range = sections.getFirst()->getEditTimeRange();

    for (auto* section : sections)
        range = range.getUnionWith (section->getEditTimeRange());

    return range;
}

//==============================================================================
void moveSection (te::ArrangerClip& section, te::BeatPosition destination)
{
    const UndoStep step (section.edit, "Move Section");
    moveTime (section.edit, getRange (section), destination);
}

te::ArrangerClip* copySection (te::ArrangerClip& section, te::BeatPosition destination, bool asAliases)
{
    auto& edit = section.edit;
    const UndoStep step (edit, asAliases ? "Copy Section as Aliases" : "Copy Section");
    copyTime (edit, getRange (section), destination, asAliases);
    return findSectionStartingAt (edit, destination, &section);
}

te::ArrangerClip* duplicateSection (te::ArrangerClip& section, bool asAliases)
{
    return copySection (section, section.getEndBeat(), asAliases);
}

void deleteSection (te::ArrangerClip& section)
{
    removeTime (section.edit, getRange (section));
}

void deleteSections (const juce::Array<te::ArrangerClip*>& sections)
{
    for (const auto& section : toSafeLatestFirst (sections))
        if (auto* s = section.get())
            deleteSection (*s);
}

juce::Array<te::ArrangerClip*> duplicateSections (const juce::Array<te::ArrangerClip*>& sections, bool asAliases,
                                                  te::SelectionManager* selection)
{
    juce::Array<te::ArrangerClip*> copies;

    if (sections.isEmpty())
        return copies;

    if (selection != nullptr)
        selection->deselectAll();

    for (const auto& section : toSafeLatestFirst (sections))
    {
        if (auto* s = section.get())
        {
            if (auto* copy = duplicateSection (*s, asAliases))
            {
                copies.add (copy);

                if (selection != nullptr)
                    selection->addToSelection (copy);
            }
        }
    }

    return copies;
}

void resizeSection (te::ArrangerClip& section, te::BeatDuration newLength, bool moveRest)
{
    auto& edit = section.edit;
    const UndoStep step (edit, "Resize Section");
    const auto range = getRange (section);
    newLength = std::max (newLength, te::BeatDuration::fromBeats (1.0));

    if (std::abs ((newLength - range.getLength()).inBeats()) < tinyBeats.inBeats())
        return;

    if (moveRest)
    {
        if (newLength > range.getLength())
        {
            insertTime (edit, range.getEnd(), newLength - range.getLength()); // moves the sections after it, not it
            section.setEnd (edit.tempoSequence.toTime (range.getStart() + newLength), true);
        }
        else
        {
            removeTime (edit, { range.getStart() + newLength, range.getEnd() }); // shortens it
        }

        return;
    }

    // The section alone, up to the next one at most.
    auto end = range.getStart() + newLength;

    for (auto* other : getSections (edit))
        if (other != &section && other->getStartBeat() >= range.getEnd() - tinyBeats)
            end = std::min (end, other->getStartBeat());

    if (end > range.getStart() + tinyBeats)
        section.setEnd (edit.tempoSequence.toTime (end), true);
}

juce::Array<te::BeatPosition> getSectionBoundaries (const te::Edit& edit)
{
    juce::Array<te::BeatPosition> boundaries;
    const auto sections = getSections (edit);

    for (auto* section : sections)
        boundaries.add (section->getStartBeat());

    if (! sections.isEmpty())
        boundaries.add (sections.getLast()->getEndBeat());

    return boundaries;
}

//==============================================================================
void addSectionCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto getEdit = [&selection] { return selection.getEdit(); };
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "section.add",
                    .name = "Add Section",
                    .category = "Section",
                    .description = "Adds an 8-bar section after the last one, or at the start of the song if it has none.",
                    .synonyms = { "new section", "part", "arranger", "marker" },
                    .perform = withEdit (selection, [&selection] (te::Edit& edit) { appendSection (edit, &selection); }),
                    .isEnabled = hasEdit,
                    .undoTarget = getEdit });
}
} // namespace phazite
