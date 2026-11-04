// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Modulation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>

namespace phazite
{
namespace
{
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
        auto& edit = parameter.getEdit();

        if (! isSongPlaying (edit))
            parameter.updateToFollowCurve (getPlayheadTime (edit));
    }

    // A channel's element in the song: a track's TRACK, or the master track's MASTER.
    juce::ValueTree findChannelState (const te::Track& track)
    {
        return track.isMasterTrack() ? findChainState (track.edit, nullptr) : findTrackState (track);
    }

    // What a modulator modulates, by the parameter, in the song (Schema.h: MODULATES); invalid for none.
    juce::ValueTree findModulatesState (const juce::ValueTree& modulator, const ParameterRef& parameter)
    {
        for (const auto& child : modulator)
            if (child.hasType (schema::MODULATES) && getModulatedParameter (child) == parameter)
                return child;

        return {};
    }

    void writeSteps (juce::ValueTree modulator, std::vector<float> steps, juce::UndoManager* undo)
    {
        while (! steps.empty() && std::abs (steps.back()) < 1.0e-4f)
            steps.pop_back();

        juce::StringArray values;

        for (const auto step : steps)
            values.add (juce::String (std::round ((double) step * 1.0e4) / 1.0e4));

        if (values.isEmpty())
            modulator.removeProperty (schema::modulatorSteps, undo);
        else
            modulator.setProperty (schema::modulatorSteps, values.joinIntoString (" "), undo);
    }
} // namespace

//==============================================================================
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

std::optional<ModulatorType> findModulatorType (const juce::String& name)
{
    for (size_t i = 0; i < std::size (schema::preset::modulatorTypeNames); ++i)
        if (name == schema::preset::modulatorTypeNames[i])
            return (ModulatorType) i;

    return {};
}

std::optional<devices::ModulatorKind> getModulatorKind (const juce::ValueTree& state)
{
    const auto type = state.hasType (schema::MODULATOR) ? findModulatorType (state[schema::modulatorType].toString()) : std::nullopt;

    if (! type.has_value())
        return {};

    switch (*type)
    {
        case ModulatorType::envelopeFollower:
            return devices::ModulatorKind::envelopeFollower;
        case ModulatorType::steps:
            return devices::ModulatorKind::steps;
        case ModulatorType::random:
            return devices::ModulatorKind::random;
        case ModulatorType::breakpoints:
            return devices::ModulatorKind::breakpoints;
        case ModulatorType::noteFollower:
            return devices::ModulatorKind::noteFollower;
        case ModulatorType::lfo:
        case ModulatorType::macro:
            break;
    }

    return devices::ModulatorKind::lfo;
}

std::vector<juce::ValueTree> getModulatorStates (const Song& song)
{
    std::vector<juce::ValueTree> states;
    auto channels = getTrackStates (song);
    channels.push_back (findChainState (song, {}));

    const auto collect = [&states] (const juce::ValueTree& owner)
    {
        for (const auto& child : owner)
            if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
                states.push_back (child);
    };

    for (const auto& channel : channels)
    {
        collect (channel);

        for (const auto& device : getDeviceStates (channel))
            if (device.hasType (schema::CONTAINER))
                collect (device);
    }

    return states;
}

juce::ValueTree findModulatorState (const Song& song, ModulatorID id)
{
    for (const auto& state : getModulatorStates (song))
        if (getModulatorID (state) == id)
            return state;

    return {};
}

std::vector<juce::ValueTree> getModulatorStates (const te::Edit& edit)
{
    return getModulatorStates (getSong (edit));
}

juce::ValueTree findModulatorState (const te::Edit& edit, ModulatorID id)
{
    return findModulatorState (getSong (edit), id);
}

ModulatorID getModulatorID (const juce::ValueTree& modulatorState)
{
    return ModulatorID::fromRaw ((std::uint64_t) (juce::int64) modulatorState[schema::modulatorID]);
}

ParameterRef getModulatedParameter (const juce::ValueTree& modulates)
{
    return { DeviceID::fromRaw (te::EditItemID::fromVar (modulates[schema::modulatesDevice]).getRawID()),
             modulates[schema::modulatesParameter].toString() };
}

std::vector<float> getModulatorSteps (const juce::ValueTree& modulatorState)
{
    std::vector<float> steps;

    for (const auto& value : juce::StringArray::fromTokens (modulatorState[schema::modulatorSteps].toString(), " ", {}))
        steps.push_back (value.getFloatValue());

    return steps;
}

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

    if (auto* rack = getContainerRack())
    {
        auto* container = findContainerOf (*rack);
        return container != nullptr ? getTrackOfContainer (*container) : nullptr;
    }

    if (macro != nullptr)
        if (auto* list = dynamic_cast<te::MacroParameterList*> (&macro->automatableEditElement))
            return list->getTrack();

    return nullptr;
}

te::RackType* ModulatorAccess::getContainerRack() const
{
    auto* list = macro != nullptr ? dynamic_cast<te::MacroParameterList*> (&macro->automatableEditElement) : nullptr;
    const auto owner = list != nullptr ? list->state.getParent() : juce::ValueTree();

    if (! owner.hasType (te::IDs::RACK))
        return nullptr;

    return macro->getEdit().getRackList().getRackTypeForID (te::EditItemID::fromID (owner)).get();
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
        {
            auto& macros = audio->getMacroParameterListForWriting();

            if (auto* macro = addMacro (macros, "Macro " + juce::String (macros.getMacroParameters().size() + 1)))
                return ModulatorAccess { nullptr, macro };
        }

        return {};
    }

    // The song's, at the end of the track's, which the mirror makes.
    auto& edit = track.edit;
    auto channel = findChannelState (track);

    if (! channel.isValid() || track.getModifierList() == nullptr)
        return {};

    const auto id = ModulatorID::fromRaw (getSong (edit).makeID());
    juce::ValueTree modulator (schema::MODULATOR);
    modulator.setProperty (schema::modulatorID, (juce::int64) id.getRaw(), nullptr);
    modulator.setProperty (schema::modulatorType, schema::preset::modulatorTypeNames[(size_t) type], nullptr);

    // Tracktion's LFOs and random sources start going one way only, and its step sequencers and envelope followers at
    // no depth, moving nothing: Phazite's start otherwise.
    juce::ValueTree values (schema::VALUES);

    if (type == ModulatorType::lfo || type == ModulatorType::random)
        values.setProperty ("biopolar", 1.0f, nullptr);
    else if (type == ModulatorType::envelopeFollower || type == ModulatorType::steps)
        values.setProperty ("depth", 1.0f, nullptr);

    if (values.getNumProperties() > 0)
        modulator.appendChild (values, nullptr);

    channel.appendChild (modulator, &getSong (edit).getUndoManager());
    mirror::reconcileModulation (edit);
    auto added = findModulator (edit, id);

    // A step sequencer's steps rise across as many steps as it starts with.
    if (auto* sequencer = added.has_value() ? dynamic_cast<te::StepModifier*> (added->modifier.get()) : nullptr)
    {
        const auto numSteps = juce::jlimit (1, (int) te::StepModifier::maxNumSteps, juce::roundToInt (sequencer->numSteps.get()));

        for (int i = 0; i < numSteps; ++i)
            setModulatorStep (*added, i, numSteps > 1 ? -1.0f + 2.0f * (float) i / (float) (numSteps - 1) : 0.0f);
    }

    return added;
}

te::MacroParameter* addMacro (te::MacroParameterList& macros, const juce::String& name)
{
    auto& edit = macros.edit;
    const auto id = ModulatorID::fromRaw (getSong (edit).makeID());

    // A track's is the song's (Schema.h: MACRO), which the mirror makes; a container's not yet on a channel (a preset's) is
    // Tracktion's until it is.
    if (auto track = macros.getTrack() != nullptr ? findTrackState (*macros.getTrack()) : juce::ValueTree(); track.isValid())
    {
        juce::ValueTree macro (schema::MACRO);
        macro.setProperty (schema::modulatorID, (juce::int64) id.getRaw(), nullptr);
        macro.setProperty (schema::macroName, name, nullptr);
        track.appendChild (macro, &getSong (edit).getUndoManager());
        mirror::reconcileModulation (edit);
    }
    else
    {
        // Made with its value in its state, so that redoing the step puts it back at 0 too.
        juce::ValueTree macro (te::IDs::MACROPARAMETER);
        macro.setProperty (te::IDs::id, (juce::int64) id.getRaw(), nullptr);
        macro.setProperty (te::IDs::name, name, nullptr);
        macro.setProperty (te::IDs::value, 0.0f, nullptr);
        macros.state.addChild (macro, -1, &edit.getUndoManager());
    }

    for (auto* added : macros.getMacroParameters())
        if (getModulatorID (*added) == id)
            return added;

    return nullptr;
}

void renameMacro (te::MacroParameter& macro, const juce::String& name)
{
    const auto trimmed = name.trim();
    auto& edit = macro.getEdit();

    if (trimmed.isEmpty())
        return;

    if (auto state = findModulatorState (edit, getModulatorID (macro)); state.isValid())
    {
        state.setProperty (schema::macroName, trimmed, &getSong (edit).getUndoManager());
        mirror::reconcileModulation (edit);
    }
    else
    {
        macro.macroName = trimmed;
    }
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
    auto* track = modulator.getTrack();

    if (track == nullptr)
        return;

    // With what it modulates (its MODULATES), which the mirror takes away with Tracktion's.
    auto& edit = track->edit;

    if (auto state = findModulatorState (edit, modulator.getID()); state.isValid())
    {
        state.getParent().removeChild (state, &getSong (edit).getUndoManager());
        mirror::reconcileModulation (edit);
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

bool isModulatorOn (const ModulatorAccess& modulator)
{
    return modulator.modifier == nullptr || isOn (modulator.modifier->enabledParam);
}

void setModulatorOn (const ModulatorAccess& modulator, bool on)
{
    auto* modifier = modulator.modifier.get();
    auto state = modifier != nullptr ? findModulatorState (modifier->edit, modulator.getID()) : juce::ValueTree();

    if (! state.isValid() || isModulatorOn (modulator) == on)
        return;

    if (on)
        state.removeProperty (schema::deviceOff, &getSong (modifier->edit).getUndoManager());
    else
        state.setProperty (schema::deviceOff, true, &getSong (modifier->edit).getUndoManager());

    mirror::reconcileModulation (modifier->edit);

    for (const auto& modulation : getModulations (modulator))
        followModulation (*modulation.parameter);
}

void setModulatorStep (const ModulatorAccess& modulator, int index, float value)
{
    auto* modifier = modulator.modifier.get();
    auto state = modifier != nullptr ? findModulatorState (modifier->edit, modulator.getID()) : juce::ValueTree();

    if (! state.isValid() || ! juce::isPositiveAndBelow (index, (int) te::StepModifier::maxNumSteps))
        return;

    auto steps = getModulatorSteps (state);
    steps.resize (std::max (steps.size(), (size_t) index + 1), 0.0f);
    steps[(size_t) index] = juce::jlimit (-1.0f, 1.0f, value);
    writeSteps (state, steps, &getSong (modifier->edit).getUndoManager());
    mirror::reconcileModulation (modifier->edit);
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
    // A macro's own value is set by hand, or by its automation.
    if (&parameter.automatableEditElement == modulator.modifier.get() || dynamic_cast<const te::MacroParameter*> (&parameter) != nullptr)
        return false;

    if (auto* rack = modulator.getContainerRack())
    {
        auto* device = parameter.getPlugin();
        return device != nullptr && device->isInRack() && device->getOwnerRackType().get() == rack;
    }

    auto* track = modulator.getTrack();
    return track != nullptr && getTrackOfParameter (parameter) == track;
}

void setModulation (const ModulatorAccess& modulator, te::AutomatableParameter& parameter, float amount)
{
    auto& edit = parameter.getEdit();
    auto state = findModulatorState (edit, modulator.getID());

    if (! state.isValid())
        return;

    // In the song, by the parameter (its device's ID as a curve names it), which the mirror plays.
    auto* undo = &getSong (edit).getUndoManager();
    const auto ref = getParameterRef (parameter);
    auto modulates = findModulatesState (state, ref);

    if (! modulates.isValid())
    {
        modulates = juce::ValueTree (schema::MODULATES);
        modulates.setProperty (schema::modulatesDevice, toTracktion (ref.device).toString(), nullptr);
        modulates.setProperty (schema::modulatesParameter, ref.parameter, nullptr);
        state.appendChild (modulates, undo);
    }

    modulates.setProperty (schema::modulatesAmount, std::round ((double) juce::jlimit (-1.0f, 1.0f, amount) * 1.0e4) / 1.0e4, undo);
    mirror::reconcileModulation (edit);
    followModulation (parameter);
}

void removeModulation (const ModulatorAccess& modulator, te::AutomatableParameter& parameter)
{
    auto& edit = parameter.getEdit();

    if (auto modulates = findModulatesState (findModulatorState (edit, modulator.getID()), getParameterRef (parameter));
        modulates.isValid())
    {
        modulates.getParent().removeChild (modulates, &getSong (edit).getUndoManager());
        mirror::reconcileModulation (edit);
    }

    followModulation (parameter);
}

//==============================================================================
namespace
{
    bool isSame (const DeviceModulation& a, const DeviceModulation& b)
    {
        const auto sameSource = [] (const DeviceModulation::Source& x, const DeviceModulation::Source& y)
        { return x.modifier == y.modifier && x.macro == y.macro; };

        const auto sameRoute = [] (const engine::Modulation::Route& x, const engine::Modulation::Route& y)
        { return x.parameter == y.parameter && x.source == y.source && juce::exactlyEqual (x.amount, y.amount); };

        const auto& routes = a.plan.routes;
        return std::equal (a.sources.begin(), a.sources.end(), b.sources.begin(), b.sources.end(), sameSource)
               && std::equal (routes.begin(), routes.end(), b.plan.routes.begin(), b.plan.routes.end(), sameRoute);
    }

    // Whether a modulation adds its source's value times its amount, as Phazite makes them: no offset or curve, and a
    // macro's whole range.
    bool isPlain (const ModulationAccess& modulation)
    {
        const auto& assignment = *modulation.assignment;
        return juce::exactlyEqual (assignment.offset.get(), 0.0f) && juce::exactlyEqual (assignment.curve.get(), 0.0f)
               && juce::exactlyEqual (assignment.inputStart.get(), 0.0f) && juce::exactlyEqual (assignment.inputEnd.get(), 1.0f);
    }
} // namespace

void updateDeviceModulation (DevicePlugin& device)
{
    const auto& info = device.getDeviceInfo();
    auto modulation = std::make_unique<DeviceModulation>();
    auto& plan = modulation->plan;
    plan.isModulated.assign (info.parameters.size(), false);

    for (size_t i = 0; i < info.parameters.size(); ++i)
    {
        auto* parameter = device.findParameter (info.parameters[i].id);

        if (parameter == nullptr)
            continue;

        // One with a modulation made otherwise, or moved by anything else of Tracktion's, stays Tracktion's to move, once a
        // block.
        const auto modulations = getModulations (*parameter);

        if ((int) modulations.size() != parameter->getAssignments().size()
            || ! std::all_of (modulations.begin(), modulations.end(), isPlain))
            continue;

        for (const auto& each : modulations)
        {
            const auto& modulator = each.modulator;
            auto& sources = modulation->sources;
            auto source = std::find_if (sources.begin(), sources.end(), [&modulator] (const DeviceModulation::Source& s)
                                        { return s.modifier == modulator.modifier && s.macro == modulator.macro; });

            if (source == sources.end())
            {
                DeviceModulation::Source added;
                added.modifier = modulator.modifier;
                added.macro = modulator.macro;
                added.lfo = dynamic_cast<te::LFOModifier*> (modulator.modifier.get());
                source = sources.insert (sources.end(), std::move (added));
                plan.sources.emplace_back();
            }

            plan.routes.push_back ({ (int) i, (int) std::distance (sources.begin(), source), each.assignment->value.get() });
            plan.isModulated[i] = true;
        }
    }

    if (plan.routes.empty())
        modulation = nullptr;

    // As it was: left playing, rather than started again.
    if (const auto* playing = device.getModulation();
        modulation == nullptr ? playing == nullptr : playing != nullptr && isSame (*playing, *modulation))
        return;

    device.setModulation (std::move (modulation));
}

void updateDeviceModulation (te::Edit& edit)
{
    for (auto* plugin : te::getAllPlugins (edit, false))
        if (auto* device = dynamic_cast<DevicePlugin*> (plugin))
            updateDeviceModulation (*device);
}

DeviceModulationKeeper::DeviceModulationKeeper (te::Edit& e)
    : edit (e),
      state (e.state)
{
    updateDeviceModulation (edit);
    state.addListener (this);
}

DeviceModulationKeeper::~DeviceModulationKeeper()
{
    state.removeListener (this);
    cancelPendingUpdate();
}

void DeviceModulationKeeper::changed (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    // Modulators, macros and what they modulate; and devices (in containers too), containers' rack types and tracks,
    // which come and go with theirs. Not what tracks and devices hold otherwise, such as clips and notes.
    const auto isModulation = [] (const juce::ValueTree& v)
    { return v.hasType (te::IDs::MODIFIERASSIGNMENTS) || v.hasType (te::IDs::MODIFIERS) || v.hasType (te::IDs::MACROPARAMETERS); };

    if (isModulation (parent) || isModulation (child) || child.hasType (te::IDs::PLUGIN) || child.hasType (te::IDs::RACK)
        || te::TrackList::isTrack (child))
        triggerAsyncUpdate();
}

void DeviceModulationKeeper::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    // Of properties, only an amount: an assignment's, in a device's MODIFIERASSIGNMENTS.
    if (tree.getParent().hasType (te::IDs::MODIFIERASSIGNMENTS))
        triggerAsyncUpdate();
}
} // namespace phazite
