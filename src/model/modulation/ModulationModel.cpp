// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulationModel.h"
#include "internal/Modulation.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/mixer/internal/Containers.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>

namespace phazite
{
namespace
{
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    std::optional<ModulatorAccess> find (const ProjectContext& context, ModulatorID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr ? findModulator (*edit, id) : std::nullopt;
    }

    te::AutomatableParameter::Ptr find (const ProjectContext& context, const ParameterRef& parameter)
    {
        auto* edit = getEdit (context);
        return edit != nullptr ? findParameter (*edit, parameter) : nullptr;
    }

    ModulatorInfo makeInfo (const ModulatorAccess& modulator)
    {
        ModulatorInfo info;
        info.id = modulator.getID();
        info.track = modulator.getTrack() != nullptr ? getTrackID (*modulator.getTrack()) : TrackID();
        info.type = getModulatorType (modulator).value_or (ModulatorType::lfo);
        info.name = getModulatorName (modulator);
        info.isOn = isModulatorOn (modulator);

        if (auto* rack = modulator.getContainerRack())
            if (auto* container = findContainerOf (*rack))
                info.container = getDeviceID (*container);

        return info;
    }

    Modulation makeModulation (const ModulationAccess& modulation)
    {
        return { modulation.modulator.getID(), getParameterRef (*modulation.parameter), modulation.assignment->value.get() };
    }

    std::vector<Modulation> makeModulations (const std::vector<ModulationAccess>& modulations)
    {
        std::vector<Modulation> made;

        for (const auto& modulation : modulations)
            made.push_back (makeModulation (modulation));

        return made;
    }

    // "Modulate Cutoff", or "Change Modulation" for one there is.
    void modulateAsStep (const ModulatorAccess& modulator, te::AutomatableParameter& parameter, float amount)
    {
        const auto modulations = getModulations (parameter);
        const auto exists = std::any_of (modulations.begin(), modulations.end(), [&modulator] (const auto& modulation)
                                         { return modulation.modulator.getID() == modulator.getID(); });
        const UndoStep step (parameter.getEdit(), exists ? "Change Modulation" : "Modulate " + getParameterName (parameter));
        setModulation (modulator, parameter, amount);
    }

    void removeAsStep (const ModulatorAccess& modulator, te::AutomatableParameter& parameter)
    {
        const UndoStep step (parameter.getEdit(), "Remove Modulation");
        removeModulation (modulator, parameter);
    }

    int getNumSteps (const te::StepModifier& sequencer)
    {
        return juce::jlimit (1, (int) te::StepModifier::maxNumSteps, juce::roundToInt (sequencer.numStepsParam->getCurrentValue()));
    }

    // The track of a handle's parameter, whose modulators can modulate it.
    te::Track* getTrackOf (const ParameterHandle& handle)
    {
        const auto parameter = findParameter (handle);
        return parameter != nullptr && parameter->getPlugin() != nullptr ? getTrackOfParameter (*parameter) : nullptr;
    }

    // A modulator by its ID in the Edit of a handle's parameter.
    std::optional<ModulatorAccess> findFor (const ParameterHandle& handle, ModulatorID id)
    {
        const auto parameter = findParameter (handle);
        return parameter != nullptr ? findModulator (parameter->getEdit(), id) : std::nullopt;
    }
} // namespace

//==============================================================================
std::vector<ModulatorInfo> getModulators (const ProjectContext& context, TrackID id)
{
    std::vector<ModulatorInfo> modulators;

    if (auto* track = findTrack (context, id))
        for (const auto& modulator : getModulators (*track))
            modulators.push_back (makeInfo (modulator));

    return modulators;
}

std::optional<ModulatorInfo> getModulator (const ProjectContext& context, ModulatorID id)
{
    if (const auto modulator = find (context, id); modulator.has_value() && modulator->getTrack() != nullptr)
        return makeInfo (*modulator);

    return {};
}

ModulatorID addModulator (ProjectContext& context, TrackID id, ModulatorType type)
{
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return {};

    const UndoStep step (track->edit, "Add " + getModulatorTypeName (type));
    const auto added = addModulator (*track, type);
    return added.has_value() ? added->getID() : ModulatorID();
}

void removeModulator (ProjectContext& context, ModulatorID id)
{
    // A container keeps its eight macros.
    if (const auto modulator = find (context, id);
        modulator.has_value() && modulator->getTrack() != nullptr && modulator->getContainerRack() == nullptr)
    {
        const UndoStep step (modulator->getTrack()->edit, "Remove " + getModulatorName (*modulator));
        removeModulator (*modulator);
    }
}

bool isModulatorOn (const ProjectContext& context, ModulatorID id)
{
    const auto modulator = find (context, id);
    return modulator.has_value() && isModulatorOn (*modulator);
}

void setModulatorOn (ProjectContext& context, ModulatorID id, bool on)
{
    if (const auto modulator = find (context, id);
        modulator.has_value() && modulator->modifier != nullptr && isModulatorOn (*modulator) != on)
    {
        const UndoStep step (modulator->modifier->edit, on ? "Turn Modulator On" : "Turn Modulator Off");
        setModulatorOn (*modulator, on);
    }
}

std::vector<ModulatorParameter> getModulatorParameters (const ProjectContext& context, ModulatorID id)
{
    std::vector<ModulatorParameter> parameters;

    if (const auto modulator = find (context, id))
    {
        if (modulator->modifier != nullptr)
            for (auto* parameter : modulator->modifier->getAutomatableParameters())
                if (parameter != modulator->modifier->enabledParam.get()) // on and off is the modulator's switch, not a knob
                    parameters.push_back ({ parameter->paramID, getModulatorParameterName (*parameter) });

        if (modulator->macro != nullptr)
            parameters.push_back ({ modulator->macro->paramID, getModulatorName (*modulator) });
    }

    return parameters;
}

ParameterHandle getModulatorParameter (ProjectContext& context, ModulatorID id, const juce::String& parameterID)
{
    const auto modulator = find (context, id);

    if (! modulator.has_value())
        return {};

    if (modulator->modifier != nullptr)
    {
        const auto parameter = modulator->modifier->getAutomatableParameterByID (parameterID);
        return parameter != nullptr
                   ? makeParameterHandle (*modulator->modifier, parameterID, "Change " + getModulatorParameterName (*parameter))
                   : ParameterHandle();
    }

    if (modulator->macro == nullptr || modulator->macro->paramID != parameterID)
        return {};

    const auto undoName = "Change " + getModulatorName (*modulator);

    if (auto* rack = modulator->getContainerRack())
        return makeMacroHandle (*rack, *modulator->macro, undoName);

    if (auto* track = dynamic_cast<te::AudioTrack*> (modulator->getTrack()))
        return makeMacroHandle (*track, *modulator->macro, undoName);

    return {};
}

void renameMacro (ProjectContext& context, ModulatorID id, const juce::String& name)
{
    const auto modulator = find (context, id);

    if (! modulator.has_value() || modulator->macro == nullptr || name.trim().isEmpty() || name.trim() == modulator->macro->macroName.get())
        return;

    const UndoStep step (modulator->macro->getEdit(), "Rename Macro");
    renameMacro (*modulator->macro, name);
}

std::vector<float> getModulatorOutput (const ProjectContext& context, ModulatorID id, double seconds)
{
    if (const auto modulator = find (context, id); modulator.has_value() && modulator->modifier != nullptr)
        return modulator->modifier->getValues (te::TimeDuration::fromSeconds (seconds));

    return {};
}

float getModulatorValue (const ProjectContext& context, ModulatorID id)
{
    if (const auto modulator = find (context, id))
    {
        if (modulator->modifier != nullptr)
            return juce::jlimit (-1.0f, 1.0f, modulator->modifier->getCurrentValue());

        if (modulator->macro != nullptr)
            return modulator->macro->getCurrentNormalisedValue();
    }

    return 0.0f;
}

bool isBipolar (const ProjectContext& context, ModulatorID id)
{
    const auto modulator = find (context, id);
    return modulator.has_value() && isBipolar (*modulator);
}

std::vector<float> getSteps (const ProjectContext& context, ModulatorID id)
{
    std::vector<float> steps;

    if (const auto modulator = find (context, id))
        if (auto* sequencer = dynamic_cast<te::StepModifier*> (modulator->modifier.get()))
            for (int i = 0; i < getNumSteps (*sequencer); ++i)
                steps.push_back (sequencer->getStep (i));

    return steps;
}

void setStep (ProjectContext& context, ModulatorID id, int index, float value)
{
    if (const auto modulator = find (context, id))
    {
        if (auto* sequencer = dynamic_cast<te::StepModifier*> (modulator->modifier.get());
            sequencer != nullptr && juce::isPositiveAndBelow (index, getNumSteps (*sequencer)))
        {
            const UndoStep step (sequencer->edit, "Change Step");
            sequencer->setStep (index, juce::jlimit (-1.0f, 1.0f, value));
        }
    }
}

//==============================================================================
std::vector<Modulation> getModulations (const ProjectContext& context, const ParameterRef& ref)
{
    const auto parameter = find (context, ref);
    return parameter != nullptr ? makeModulations (getModulations (*parameter)) : std::vector<Modulation>();
}

std::vector<Modulation> getModulations (const ProjectContext& context, ModulatorID id)
{
    const auto modulator = find (context, id);
    return modulator.has_value() ? makeModulations (getModulations (*modulator)) : std::vector<Modulation>();
}

bool canModulate (const ProjectContext& context, ModulatorID id, const ParameterRef& ref)
{
    const auto modulator = find (context, id);
    const auto parameter = find (context, ref);
    return modulator.has_value() && parameter != nullptr && canModulate (*modulator, *parameter);
}

void modulate (ProjectContext& context, ModulatorID id, const ParameterRef& ref, float amount)
{
    const auto modulator = find (context, id);

    if (const auto parameter = find (context, ref); modulator.has_value() && parameter != nullptr && canModulate (*modulator, *parameter))
        modulateAsStep (*modulator, *parameter, amount);
}

void removeModulation (ProjectContext& context, ModulatorID id, const ParameterRef& ref)
{
    const auto modulator = find (context, id);

    if (const auto parameter = find (context, ref); modulator.has_value() && parameter != nullptr)
        removeAsStep (*modulator, *parameter);
}

//==============================================================================
std::vector<ModulatorInfo> getModulatorsFor (const ParameterHandle& handle)
{
    std::vector<ModulatorInfo> modulators;

    // A device's in a container: its container's macros first.
    if (const auto parameter = findParameter (handle))
        if (auto* device = parameter->getPlugin(); device != nullptr && device->isInRack())
            if (auto* container = findContainerOf (*device))
                for (auto* macro : getContainerMacros (*container))
                    modulators.push_back (makeInfo ({ nullptr, macro }));

    if (auto* track = getTrackOf (handle))
        for (const auto& modulator : getModulators (*track))
            modulators.push_back (makeInfo (modulator));

    return modulators;
}

std::vector<Modulation> getModulations (const ParameterHandle& handle)
{
    const auto parameter = findParameter (handle);
    return parameter != nullptr ? makeModulations (getModulations (*parameter)) : std::vector<Modulation>();
}

ModulatorID modulateWithNew (const ParameterHandle& handle, ModulatorType type, float amount)
{
    const auto parameter = findParameter (handle);
    auto* track = getTrackOf (handle);

    if (parameter == nullptr || track == nullptr)
        return {};

    const UndoStep step (parameter->getEdit(), "Modulate " + getParameterName (*parameter));
    const auto added = addModulator (*track, type);

    if (! added.has_value() || ! canModulate (*added, *parameter))
        return {};

    setModulation (*added, *parameter, amount);
    return added->getID();
}

void modulate (const ParameterHandle& handle, ModulatorID id, float amount)
{
    const auto parameter = findParameter (handle);

    if (const auto modulator = findFor (handle, id); parameter != nullptr && modulator.has_value() && canModulate (*modulator, *parameter))
        modulateAsStep (*modulator, *parameter, amount);
}

void removeModulation (const ParameterHandle& handle, ModulatorID id)
{
    const auto parameter = findParameter (handle);

    if (const auto modulator = findFor (handle, id); parameter != nullptr && modulator.has_value())
        removeAsStep (*modulator, *parameter);
}

//==============================================================================
struct ModulationDrag::Impl
{
    Impl (te::AutomatableParameter& p, ModulatorAccess m, float amount)
        : parameter (&p),
          modulator (std::move (m)),
          startAmount (amount),
          gesture (p.getEdit(), "Change Modulation")
    {
    }

    te::AutomatableParameter::Ptr parameter;
    ModulatorAccess modulator;
    float startAmount = 0.0f;
    Gesture gesture;
};

ModulationDrag::ModulationDrag (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

ModulationDrag::~ModulationDrag()
{
    impl->gesture.commit();
}

float ModulationDrag::getStartAmount() const noexcept
{
    return impl->startAmount;
}

void ModulationDrag::setAmount (float amount)
{
    setModulation (impl->modulator, *impl->parameter, amount);
}

std::unique_ptr<ModulationDrag> beginModulationDrag (const ParameterHandle& handle, ModulatorID id)
{
    const auto parameter = findParameter (handle);
    const auto modulator = findFor (handle, id);

    if (parameter == nullptr || ! modulator.has_value())
        return {};

    for (const auto& modulation : getModulations (*parameter))
        if (modulation.modulator.getID() == id)
            return std::make_unique<ModulationDrag> (
                std::make_unique<ModulationDrag::Impl> (*parameter, *modulator, modulation.assignment->value.get()));

    return {};
}

std::optional<ModulationRange> getModulationRange (const ParameterHandle& handle)
{
    const auto parameter = findParameter (handle);

    if (parameter == nullptr)
        return {};

    const auto modulations = getModulations (*parameter);

    if (modulations.empty())
        return {};

    // From where its value is without modulation, each modulation's reach: a bipolar modulator's both ways. Only those
    // whose modulators are on, unless all are off.
    const auto anyOn = std::any_of (modulations.begin(), modulations.end(), [] (const auto& m) { return isModulatorOn (m.modulator); });
    const auto& range = parameter->valueRange;
    const auto base = range.convertTo0to1 (range.snapToLegalValue (parameter->getCurrentBaseValue()));
    auto low = base, high = base;

    for (const auto& modulation : modulations)
    {
        if (anyOn && ! isModulatorOn (modulation.modulator))
            continue;

        const auto amount = modulation.assignment->value.get();
        const auto reach = isBipolar (modulation.modulator) ? std::abs (amount) : 0.0f;
        low += std::min ({ amount, -reach, 0.0f });
        high += std::max ({ amount, reach, 0.0f });
    }

    return ModulationRange { juce::jlimit (0.0f, 1.0f, low), juce::jlimit (0.0f, 1.0f, high), parameter->getCurrentNormalisedValue(),
                             anyOn };
}
} // namespace phazite
