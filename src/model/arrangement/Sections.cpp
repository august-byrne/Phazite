// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Sections.h"
#include "Arrangement.h"
#include "Outlines.h"
#include "Tracks.h"
#include "model/commands/CommandRegistry.h"

#include <algorithm>

namespace phazite
{
namespace
{
    const juce::Identifier roleProperty { "phaziteRole" }, tipProperty { "phaziteTip" }, energyProperty { "phaziteEnergy" };
    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);

    te::BeatRange getRange (const te::Clip& section)
    {
        return { section.getStartBeat(), section.getEndBeat() };
    }

    juce::Colour getPaletteColour (const char* name)
    {
        for (const auto& colour : getTrackColours())
            if (juce::String (colour.name) == name)
                return colour.colour;

        return getTrackColours()[1].colour;
    }

    // The section that starts at a beat, other than one to leave out.
    te::ArrangerClip* findSectionStartingAt (const te::Edit& edit, te::BeatPosition beat, const te::Clip* except)
    {
        for (auto* section : getSections (edit))
            if (section != except && std::abs ((section->getStartBeat() - beat).inBeats()) < 1.0e-4)
                return section;

        return nullptr;
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

juce::String getSectionRole (const te::Clip& section)
{
    return section.state[roleProperty].toString();
}

juce::String getSectionTip (const te::Clip& section)
{
    return section.state[tipProperty].toString();
}

float getSectionEnergy (const te::Clip& section)
{
    return juce::jlimit (0.0f, 1.0f, (float) static_cast<double> (section.state.getProperty (energyProperty, 0.5)));
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

    for (const auto& entry : roleColours)
        if (role == entry.role)
            return getPaletteColour (entry.colour);

    return getPaletteColour ("Sky blue");
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
    section->state.setProperty (roleProperty, info.role, undoManager);
    section->state.setProperty (tipProperty, info.tip, undoManager);
    section->state.setProperty (energyProperty, juce::jlimit (0.0f, 1.0f, info.energy), undoManager);
    return section;
}

//==============================================================================
void moveSection (te::ArrangerClip& section, te::BeatPosition destination)
{
    moveTime (section.edit, getRange (section), destination);
}

te::ArrangerClip* copySection (te::ArrangerClip& section, te::BeatPosition destination, bool asAliases)
{
    auto& edit = section.edit;
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

void resizeSection (te::ArrangerClip& section, te::BeatDuration newLength, bool moveRest)
{
    auto& edit = section.edit;
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
                    .perform =
                        [&selection]
                    {
                        auto* edit = selection.getEdit();

                        if (edit == nullptr)
                            return;

                        const auto sections = getSections (*edit);
                        const auto start = sections.isEmpty() ? te::BeatPosition() : sections.getLast()->getEndBeat();
                        const auto& tempo = edit->tempoSequence;
                        const auto bar = tempo.toBarsAndBeats (tempo.toTime (start)).bars;
                        const auto end = tempo.toBeats (te::tempo::BarsAndBeats { bar + 8, {}, 0 });

                        if (auto* section = insertSection (*edit, start, { "Section " + juce::String (sections.size() + 1), end - start }))
                            selection.selectOnly (section);
                    },
                    .isEnabled = hasEdit,
                    .undoTarget = getEdit });

    // One command for each outline, so the palette finds them by name.
    for (const auto& outline : getBuiltInOutlines())
    {
        const auto id = outline.id;
        registry.add ({ .id = "section.outline." + id,
                        .name = "Apply Outline: " + outline.name,
                        .category = "Section",
                        .description = "Lays out the song's sections. " + outline.description
                                       + " It replaces the sections there are; what is on the tracks stays where it is.",
                        .synonyms = { "song outline", "structure", "template", "arrangement", "form" },
                        .perform =
                            [&selection, id]
                        {
                            if (auto* edit = selection.getEdit())
                                if (const auto* chosen = findOutline (id))
                                    applyOutline (*edit, *chosen);
                        },
                        .isEnabled = hasEdit,
                        .undoTarget = getEdit,
                        .isTicked =
                            [&selection, id]
                        {
                            auto* edit = selection.getEdit();
                            return edit != nullptr && getAppliedOutline (*edit) == id && ! getSections (*edit).isEmpty();
                        } });
    }
}
} // namespace phazite
