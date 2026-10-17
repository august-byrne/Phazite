// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Modulation.h"
#include "model/automation/internal/Automation.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Tracktion's modifiers, by the modulators' types.
    const juce::Identifier& getModifierType (ModulatorType type)
    {
        switch (type)
        {
            case ModulatorType::envelopeFollower:
                return te::IDs::ENVELOPEFOLLOWER;
            case ModulatorType::steps:
                return te::IDs::STEP;
            case ModulatorType::random:
                return te::IDs::RANDOM;
            case ModulatorType::breakpoints:
                return te::IDs::BREAKPOINTOSCILLATOR;
            case ModulatorType::noteFollower:
                return te::IDs::MIDITRACKER;
            case ModulatorType::lfo:
            case ModulatorType::macro:
            default:
                return te::IDs::LFO;
        }
    }

    std::optional<ModulatorType> getModulatorType (const te::Modifier& modifier)
    {
        for (const auto type : allModulatorTypes)
            if (type != ModulatorType::macro && modifier.state.hasType (getModifierType (type)))
                return type;

        return {};
    }

    bool isOn (const te::AutomatableParameter::Ptr& parameter)
    {
        return parameter != nullptr && parameter->getCurrentValue() >= 0.5f;
    }

    // Whether a modulator's assignment belongs to this one.
    bool isFrom (const te::AutomatableParameter::ModifierAssignment& assignment, const ModulatorAccess& modulator)
    {
        auto* source = modulator.getSource();
        return source != nullptr && assignment.isForModifierSource (*source);
    }

    // While the song doesn't play, a parameter shows its value as modulated where the playhead is.
    void followModulation (te::AutomatableParameter& parameter)
    {
        auto& transport = parameter.getEdit().getTransport();

        if (! transport.isPlaying())
            parameter.updateToFollowCurve (transport.getPosition());
    }
} // namespace

//==============================================================================
te::AutomatableParameter::ModifierSource* ModulatorAccess::getSource() const
{
    if (modifier != nullptr)
        return modifier.get();

    return macro.get();
}

te::Track* ModulatorAccess::getTrack() const
{
    if (modifier != nullptr)
        return te::getTrackContainingModifier (modifier->edit, modifier);

    if (macro != nullptr)
        if (auto* list = dynamic_cast<te::MacroParameterList*> (&macro->automatableEditElement))
            return list->getTrack();

    return nullptr;
}

ModulatorID ModulatorAccess::getID() const
{
    if (modifier != nullptr)
        return getModulatorID (*modifier);

    return macro != nullptr ? getModulatorID (*macro) : ModulatorID();
}

ModulatorID getModulatorID (const te::Modifier& modifier)
{
    return ModulatorID::fromRaw (modifier.itemID.getRawID());
}

ModulatorID getModulatorID (const te::MacroParameter& macro)
{
    // A macro's parameter ID is its item's ID.
    return ModulatorID::fromRaw (te::EditItemID::fromVar (macro.paramID).getRawID());
}

std::optional<ModulatorAccess> findModulator (const te::Edit& edit, ModulatorID id)
{
    if (! id.isValid())
        return {};

    const auto itemID = te::EditItemID::fromRawID (id.getRaw());

    if (auto modifier = te::findModifierForID (edit, itemID))
        return ModulatorAccess { modifier, nullptr };

    if (auto macro = te::getMacroParameterForID (const_cast<te::Edit&> (edit), itemID))
        return ModulatorAccess { nullptr, macro };

    return {};
}

std::vector<ModulatorAccess> getModulators (te::Track& track)
{
    std::vector<ModulatorAccess> modulators;

    if (auto* list = track.getModifierList())
        for (auto* modifier : list->getModifiers())
            if (getModulatorType (*modifier).has_value())
                modulators.push_back ({ modifier, nullptr });

    // Tracktion keeps macros on tracks that hold clips (and returns), not on folders or the master.
    if (auto* audio = dynamic_cast<te::AudioTrack*> (&track))
        for (auto* macro : audio->getMacroParameters())
            modulators.push_back ({ nullptr, macro });

    return modulators;
}

std::optional<ModulatorType> getModulatorType (const ModulatorAccess& modulator)
{
    if (modulator.modifier != nullptr)
        return getModulatorType (*modulator.modifier);

    return modulator.macro != nullptr ? std::optional (ModulatorType::macro) : std::nullopt;
}

juce::String getModulatorName (const ModulatorAccess& modulator)
{
    // A macro has a name of its own, "Macro 1" at first.
    if (modulator.macro != nullptr && modulator.macro->macroName.get().isNotEmpty())
        return modulator.macro->macroName.get();

    const auto type = getModulatorType (modulator);
    auto* track = modulator.getTrack();

    if (! type.has_value() || track == nullptr)
        return {};

    // Its place among its track's of its kind.
    int number = 0;

    for (const auto& each : getModulators (*track))
    {
        if (getModulatorType (each) == type)
            ++number;

        if (each.getID() == modulator.getID())
            break;
    }

    return getModulatorTypeName (*type) + " " + juce::String (number);
}

std::optional<ModulatorAccess> addModulator (te::Track& track, ModulatorType type)
{
    if (type == ModulatorType::macro)
    {
        if (auto* audio = dynamic_cast<te::AudioTrack*> (&track))
            if (auto* macro = audio->getMacroParameterListForWriting().createMacroParameter())
                return ModulatorAccess { nullptr, macro };

        return {};
    }

    auto* list = track.getModifierList();
    const auto modifier = list != nullptr ? list->insertModifier (juce::ValueTree (getModifierType (type)), -1, nullptr) : nullptr;

    if (modifier == nullptr)
        return {};

    // Tracktion's LFOs and random sources start going one way only, and its step sequencers and envelope followers at
    // no depth, moving nothing.
    // Set through the parameters, which play what they are set to.
    if (auto* lfo = dynamic_cast<te::LFOModifier*> (modifier.get()))
        lfo->bipolarParam->setParameter (1.0f, juce::sendNotification);
    else if (auto* random = dynamic_cast<te::RandomModifier*> (modifier.get()))
        random->bipolarParam->setParameter (1.0f, juce::sendNotification);
    else if (auto* follower = dynamic_cast<te::EnvelopeFollowerModifier*> (modifier.get()))
        follower->depthParam->setParameter (1.0f, juce::sendNotification);
    else if (auto* sequencer = dynamic_cast<te::StepModifier*> (modifier.get()))
    {
        sequencer->depthParam->setParameter (1.0f, juce::sendNotification);
        const auto numSteps = juce::jlimit (1, (int) te::StepModifier::maxNumSteps, juce::roundToInt (sequencer->numSteps.get()));

        for (int i = 0; i < numSteps; ++i)
            sequencer->setStep (i, numSteps > 1 ? -1.0f + 2.0f * (float) i / (float) (numSteps - 1) : 0.0f);
    }

    return ModulatorAccess { modifier, nullptr };
}

juce::String getModulatorParameterName (const te::AutomatableParameter& parameter)
{
    const auto& id = parameter.paramID;

    if (id == "biopolar")
        return "Polarity";

    if (id == "syncType")
        return "Sync";

    if (id == "numActivePoints")
        return "Points";

    if (id == "stageZeroValue")
        return "Start Level";

    // The breakpoint envelope's points: "stageTwoTime" is "Point 2 Time".
    const juce::StringArray stages { "One", "Two", "Three", "Four" };

    for (int i = 0; i < stages.size(); ++i)
        for (const auto* part : { "Value", "Time", "Curve" })
            if (id == "stage" + stages[i] + part)
                return "Point " + juce::String (i + 1) + " "
                       + (juce::String (part) == "Value" ? juce::String ("Level") : juce::String (part));

    if (id == "type" && dynamic_cast<const te::MIDITrackerModifier*> (&parameter.automatableEditElement) != nullptr)
        return "Follows";

    if (id == "lowPassEnabled")
        return "Low Pass";

    if (id == "highPassEnabled")
        return "High Pass";

    if (id == "lowPassFrequency")
        return "Low Pass Frequency";

    if (id == "highPassFrequency")
        return "High Pass Frequency";

    return parameter.getParameterName();
}

void removeModulator (const ModulatorAccess& modulator)
{
    if (modulator.modifier != nullptr)
    {
        modulator.modifier->remove(); // and its assignments
        return;
    }

    if (modulator.macro != nullptr)
    {
        for (const auto& modulation : getModulations (modulator))
            modulation.parameter->removeModifier (*modulation.assignment);

        if (auto* list = dynamic_cast<te::MacroParameterList*> (&modulator.macro->automatableEditElement))
            list->removeMacroParameter (*modulator.macro);
    }
}

bool isBipolar (const ModulatorAccess& modulator)
{
    auto* modifier = modulator.modifier.get();

    if (auto* lfo = dynamic_cast<te::LFOModifier*> (modifier))
        return isOn (lfo->bipolarParam);

    if (auto* random = dynamic_cast<te::RandomModifier*> (modifier))
        return isOn (random->bipolarParam);

    if (auto* breakpoints = dynamic_cast<te::BreakpointOscillatorModifier*> (modifier))
        return isOn (breakpoints->bipolarParam);

    return dynamic_cast<te::StepModifier*> (modifier) != nullptr; // steps go from -1 to 1
}

//==============================================================================
std::vector<ModulationAccess> getModulations (const te::AutomatableParameter& parameter)
{
    std::vector<ModulationAccess> modulations;
    auto& edit = parameter.getEdit();

    for (auto* assignment : parameter.getAssignments())
    {
        // Each assignment names its source in its state.
        const auto source = te::EditItemID::fromProperty (assignment->state, te::IDs::source);

        if (auto modulator = findModulator (edit, ModulatorID::fromRaw (source.getRawID()));
            modulator.has_value() && isFrom (*assignment, *modulator))
            modulations.push_back ({ *modulator, &const_cast<te::AutomatableParameter&> (parameter), assignment });
    }

    return modulations;
}

std::vector<ModulationAccess> getModulations (const ModulatorAccess& modulator)
{
    std::vector<ModulationAccess> modulations;

    if (auto* source = modulator.getSource(); source != nullptr && modulator.getTrack() != nullptr)
        for (auto* parameter : te::getAllParametersBeingModifiedBy (modulator.getTrack()->edit, *source))
            for (const auto& modulation : getModulations (*parameter))
                if (modulation.modulator.getID() == modulator.getID())
                    modulations.push_back (modulation);

    return modulations;
}

bool canModulate (const ModulatorAccess& modulator, const te::AutomatableParameter& parameter)
{
    auto* track = modulator.getTrack();

    if (track == nullptr || &parameter.automatableEditElement == modulator.modifier.get())
        return false;

    // A macro's own value is set by hand, or by its automation.
    if (dynamic_cast<const te::MacroParameter*> (&parameter) != nullptr)
        return false;

    return getTrackOfParameter (parameter) == track;
}

void setModulation (const ModulatorAccess& modulator, te::AutomatableParameter& parameter, float amount)
{
    auto* source = modulator.getSource();

    if (source == nullptr)
        return;

    const auto limited = juce::jlimit (-1.0f, 1.0f, amount);
    bool found = false;

    for (const auto& modulation : getModulations (parameter))
    {
        if (modulation.modulator.getID() == modulator.getID())
        {
            modulation.assignment->value = limited;
            found = true;
        }
    }

    if (! found)
        parameter.addModifier (*source, limited);

    followModulation (parameter);
}

void removeModulation (const ModulatorAccess& modulator, te::AutomatableParameter& parameter)
{
    for (const auto& modulation : getModulations (parameter))
        if (modulation.modulator.getID() == modulator.getID())
            parameter.removeModifier (*modulation.assignment);

    followModulation (parameter);
}
} // namespace phazite
