// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"

#include <algorithm>

namespace phazite::mirror
{
namespace
{

    std::vector<juce::ValueTree> getChildren (const juce::ValueTree& owner, const juce::Identifier& type)
    {
        std::vector<juce::ValueTree> children;

        for (const auto& child : owner)
            if (child.hasType (type))
                children.push_back (child);

        return children;
    }

    // A modulator plays what the song has it set to: on or off, its values set by hand (those it has; one it hasn't stays
    // at Tracktion's default, where a modulator made from the song starts), and a step sequencer's steps.
    void reconcileSettings (te::Modifier& modifier, const juce::ValueTree& state)
    {
        if (const auto on = ! (bool) state[schema::deviceOff]; (modifier.enabledParam->getCurrentExplicitValue() >= 0.5f) != on)
        {
            setValueSetByHand (*modifier.enabledParam, on ? 1.0f : 0.0f);
        }

        const auto values = state.getChildWithName (schema::VALUES);

        for (int i = 0; i < values.getNumProperties(); ++i)
        {
            const auto id = values.getPropertyName (i);

            if (auto parameter = modifier.getAutomatableParameterByID (id.toString());
                parameter != nullptr && ! juce::exactlyEqual (parameter->getCurrentExplicitValue(), (float) values[id]))
                setValueSetByHand (*parameter, (float) values[id]);
        }

        if (auto* sequencer = dynamic_cast<te::StepModifier*> (&modifier))
        {
            const auto steps = getModulatorSteps (state);

            for (int i = 0; i < (int) te::StepModifier::maxNumSteps; ++i)
                if (const auto wanted = i < (int) steps.size() ? steps[(size_t) i] : 0.0f;
                    ! juce::exactlyEqual (sequencer->getStep (i), wanted))
                    sequencer->setStep (i, wanted);
        }
    }

    // A track's modulators in Tracktion as the song has them (Schema.h: MODULATOR): those it hasn't go, with what they
    // modulate; those it has are made (of their kind, with their IDs), each in its place, set as the song has it.
    void reconcileModulators (te::Edit& edit, te::Track& track, const juce::ValueTree& channel)
    {
        auto* list = track.getModifierList();

        if (list == nullptr)
            return;

        const auto wanted = getChildren (channel, schema::MODULATOR);
        const auto findModifier = [list] (ModulatorID id) -> te::Modifier*
        {
            for (auto* modifier : list->getModifiers())
                if (getModulatorID (*modifier) == id)
                    return modifier;

            return nullptr;
        };

        for (auto* modifier : list->getModifiers())
            if (std::none_of (wanted.begin(), wanted.end(),
                              [modifier] (const juce::ValueTree& state) { return getModulatorID (state) == getModulatorID (*modifier); }))
                modifier->remove();

        for (size_t i = 0; i < wanted.size(); ++i)
        {
            const auto id = getModulatorID (wanted[i]);
            auto* modifier = findModifier (id);

            if (modifier == nullptr)
            {
                if (const auto type = findModulatorType (wanted[i][schema::modulatorType].toString()))
                {
                    juce::ValueTree made (getModifierType (*type));
                    toTracktion (id).writeID (made, nullptr);
                    list->insertModifier (made, (int) i, nullptr);
                }

                modifier = findModifier (id);

                if (modifier == nullptr)
                    continue;
            }

            if (const auto modifiers = list->getModifiers(); (int) i < modifiers.size() && modifiers[(int) i] != modifier)
                list->state.moveChild (list->state.indexOf (modifier->state), list->state.indexOf (modifiers[(int) i]->state),
                                       &edit.getUndoManager());

            reconcileSettings (*modifier, wanted[i]);
        }
    }

    // The macros in one of Tracktion's lists (a track's, or a container's rack type's) as the song has them (Schema.h:
    // MACRO): those it hasn't go, with what they modulate; those it has are made, with their IDs, each in its place, with
    // its name and value.
    void reconcileMacros (te::Edit& edit, te::MacroParameterList& list, const juce::ValueTree& owner)
    {
        auto* undo = &edit.getUndoManager();
        const auto wanted = getChildren (owner, schema::MACRO);
        const auto findMacro = [&list] (ModulatorID id) -> te::MacroParameter*
        {
            for (auto* macro : list.getMacroParameters())
                if (getModulatorID (*macro) == id)
                    return macro;

            return nullptr;
        };

        for (auto* macro : list.getMacroParameters())
        {
            if (std::none_of (wanted.begin(), wanted.end(),
                              [macro] (const juce::ValueTree& state) { return getModulatorID (state) == getModulatorID (*macro); }))
            {
                for (const auto& modulation : getModulations (ModulatorAccess { nullptr, macro }))
                    modulation.parameter->removeModifier (*modulation.assignment);

                list.removeMacroParameter (*macro);
            }
        }

        for (size_t i = 0; i < wanted.size(); ++i)
        {
            const auto id = getModulatorID (wanted[i]);
            const auto name = wanted[i][schema::macroName].toString();
            const auto value = (float) wanted[i].getProperty (schema::macroValue, 0.0f);

            // Made with its value in its state, so that redoing what made it puts it back where it was too.
            if (findMacro (id) == nullptr)
            {
                juce::ValueTree made (te::IDs::MACROPARAMETER);
                toTracktion (id).writeID (made, nullptr);
                made.setProperty (te::IDs::name, name, nullptr);
                made.setProperty (te::IDs::value, value, nullptr);
                list.state.addChild (made, std::min ((int) i, list.state.getNumChildren()), undo);
            }

            auto* macro = findMacro (id);

            if (macro == nullptr)
                continue;

            if (const auto macros = list.getMacroParameters(); (int) i < macros.size() && macros[(int) i] != macro)
                list.state.moveChild (list.state.indexOf (macro->state), list.state.indexOf (macros[(int) i]->state), undo);

            if (macro->macroName.get() != name)
                macro->macroName = name;

            if (! juce::exactlyEqual (macro->getCurrentExplicitValue(), value))
                setValueSetByHand (*macro, value);
        }
    }

    // The macros of a chain's containers, in their rack types.
    void reconcileContainerMacros (te::Edit& edit, const te::PluginList& list, const juce::ValueTree& chain)
    {
        for (const auto& state : getDeviceStates (chain))
            if (state.hasType (schema::CONTAINER))
                for (auto* device : list)
                    if (auto* rack = getContainerRack (*device); rack != nullptr && getDeviceID (*device) == getDeviceID (state))
                        reconcileMacros (edit, rack->getMacroParameterListForWriting(), state);
    }

    // What the song's modulators and macros modulate, by their amounts (Schema.h: MODULATES): what they modulate in Tracktion
    // that the song hasn't goes, and what the song has that they don't is made; and what anything of Tracktion's that the
    // song hasn't modulates goes too (the macros of a container's rack type left from an ungrouped container).
    void reconcileAssignments (te::Edit& edit)
    {
        const auto states = getModulatorStates (edit);
        std::vector<ModulatorID> ids;

        for (const auto& state : states)
            ids.push_back (getModulatorID (state));

        for (auto* parameter : edit.getAllAutomatableParams (true))
            for (auto* assignment : parameter->getAssignments())
                if (const auto source = ModulatorID::fromRaw (te::EditItemID::fromProperty (assignment->state, te::IDs::source).getRawID());
                    std::find (ids.begin(), ids.end(), source) == ids.end())
                    parameter->removeModifier (*assignment);

        for (const auto& state : states)
        {
            const auto modulator = findModulator (edit, getModulatorID (state));
            auto* source = modulator.has_value() ? modulator->getSource() : nullptr;

            if (source == nullptr)
                continue;

            std::vector<std::pair<ParameterRef, float>> wanted;

            for (const auto& child : getChildren (state, schema::MODULATES))
                wanted.push_back ({ getModulatedParameter (child), (float) child[schema::modulatesAmount] });

            for (const auto& modulation : getModulations (*modulator))
            {
                const auto ref = getParameterRef (*modulation.parameter);
                const auto found = std::find_if (wanted.begin(), wanted.end(), [&ref] (const auto& each) { return each.first == ref; });

                if (found == wanted.end())
                    modulation.parameter->removeModifier (*modulation.assignment);
                else if (! juce::exactlyEqual (modulation.assignment->value.get(), found->second))
                    modulation.assignment->value = found->second;
            }

            for (const auto& [ref, amount] : wanted)
            {
                const auto parameter = findParameter (edit, ref);

                if (parameter == nullptr)
                    continue;

                const auto modulations = getModulations (*parameter);

                if (std::none_of (modulations.begin(), modulations.end(),
                                  [&modulator] (const ModulationAccess& each) { return each.modulator.getID() == modulator->getID(); }))
                    parameter->addModifier (*source, amount);
            }
        }
    }
} // namespace

void reconcileModulation (te::Edit& edit)
{
    for (const auto& state : getTrackStates (edit))
    {
        auto* track = findTrack (edit, state);

        if (track == nullptr)
            continue;

        reconcileModulators (edit, *track, state);

        // A track's macros, on one that holds clips: its list made only for the song's first.
        if (auto* audio = dynamic_cast<te::AudioTrack*> (track))
            if (audio->getMacroParameterList() != nullptr || ! getChildren (state, schema::MACRO).empty())
                reconcileMacros (edit, audio->getMacroParameterListForWriting(), state);

        reconcileContainerMacros (edit, track->pluginList, state);
    }

    if (const auto master = findChainState (edit, nullptr); master.isValid())
    {
        if (auto* track = edit.getMasterTrack())
            reconcileModulators (edit, *track, master);

        reconcileContainerMacros (edit, edit.getMasterPluginList(), master);
    }

    reconcileAssignments (edit);
}

void reconcileModulator (te::Edit& edit, ModulatorID id)
{
    const auto state = findModulatorState (getSong (edit), id);
    const auto found = state.isValid() ? findModulator (edit, id) : std::nullopt;

    if (! found.has_value())
        return;

    if (found->modifier != nullptr)
    {
        reconcileSettings (*found->modifier, state);
    }
    else if (auto* macro = found->macro.get())
    {
        if (const auto name = state[schema::macroName].toString(); macro->macroName.get() != name)
            macro->macroName = name;

        if (const auto value = (float) state.getProperty (schema::macroValue, 0.0f);
            ! juce::exactlyEqual (macro->getCurrentExplicitValue(), value))
            setValueSetByHand (*macro, value);
    }
}
} // namespace phazite::mirror
