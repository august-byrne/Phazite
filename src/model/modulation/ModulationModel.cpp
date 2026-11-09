// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulationModel.h"
#include "internal/Modulation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // Whether any of the song's modulators or macros modulates a parameter (Schema.h: MODULATES): most parameters none
    // does, which a knob asks at each tick.
    bool isModulated (const Song& song, const ParameterRef& parameter)
    {
        for (const auto& modulator : getModulatorStates (song))
            for (const auto& child : modulator)
                if (child.hasType (schema::MODULATES) && getModulatedParameter (child) == parameter)
                    return true;

        return false;
    }

    // What plays a modulator or a macro while Phazite's engine plays the song, or nullptr.
    engine::ModulatorNode* findPlayedModulator (const Song& song, ModulatorID id)
    {
        auto* session = findPlayingSession (song);
        return session != nullptr ? session->getPlayer().findModulator ((std::int64_t) id.getRaw()) : nullptr;
    }

    ModulatorInfo makeInfo (const Song& song, ModulatorID id)
    {
        ModulatorInfo info;
        info.id = id;
        info.track = getModulatorTrack (song, id);
        info.container = getModulatorContainer (song, id);
        info.type = getModulatorType (song, id).value_or (ModulatorType::lfo);
        info.name = getModulatorName (song, id);
        info.isOn = isModulatorOn (song, id);
        return info;
    }

    // A modulator's or a macro's parameter by its reference: a modulator's by the modulator's ID, and a macro's value by
    // the macro's ID as its parameter's too.
    ParameterRef getRef (ModulatorID id, const juce::String& parameterID)
    {
        return { DeviceID::fromRaw (id.getRaw()), parameterID };
    }

    juce::String getMacroParameterID (ModulatorID id)
    {
        return juce::String (id.getRaw());
    }

    bool isModulating (const Song& song, ModulatorID modulator, const ParameterRef& parameter)
    {
        const auto modulations = getModulations (song, parameter);
        return std::any_of (modulations.begin(), modulations.end(),
                            [modulator] (const Modulation& modulation) { return modulation.modulator == modulator; });
    }

    // "Modulate Cutoff", or "Change Modulation" for one there is.
    void modulateAsStep (Song& song, ModulatorID modulator, const ParameterRef& parameter, float amount)
    {
        const UndoStep step (song, isModulating (song, modulator, parameter) ? juce::String ("Change Modulation")
                                                                             : "Modulate " + getParameterName (song, parameter));
        setModulation (song, modulator, parameter, amount);
    }

    void removeAsStep (Song& song, ModulatorID modulator, const ParameterRef& parameter)
    {
        if (isModulating (song, modulator, parameter))
        {
            const UndoStep step (song, "Remove Modulation");
            removeModulation (song, modulator, parameter);
        }
    }

    // A step sequencer's element, and how many steps it plays; invalid for another modulator.
    juce::ValueTree findSequencer (const Song& song, ModulatorID id)
    {
        const auto state = findModulatorState (song, id);
        return getModulatorKind (state) == devices::ModulatorKind::steps ? state : juce::ValueTree();
    }

    int getNumSteps (const juce::ValueTree& sequencer)
    {
        constexpr auto kind = devices::ModulatorKind::steps;
        const auto index = devices::findModulatorParameter (kind, "numSteps");
        const auto value = readValueSetByHand (sequencer, devices::getModulatorParameters (kind)[(size_t) index]);
        return juce::jlimit (1, devices::Modulator::maxSteps, juce::roundToInt (value));
    }

    // A handle's parameter in its song, while there is one, by the reference automation and modulation name it with.
    std::optional<std::pair<Song*, ParameterRef>> findTarget (const ParameterHandle& handle)
    {
        auto* song = findSong (handle);
        const auto ref = handle.getRef();

        if (song == nullptr || ! ref.isValid())
            return {};

        return std::pair { song, ref };
    }
} // namespace

//==============================================================================
std::vector<ModulatorInfo> getModulators (const ProjectContext& context, TrackID id)
{
    std::vector<ModulatorInfo> modulators;

    if (auto* song = findSong (context); song != nullptr && id.isValid() && findTrackState (*song, id).isValid())
        for (const auto modulator : getModulators (*song, id))
            modulators.push_back (makeInfo (*song, modulator));

    return modulators;
}

std::optional<ModulatorInfo> getModulator (const ProjectContext& context, ModulatorID id)
{
    if (auto* song = findSong (context); song != nullptr && getModulatorTrack (*song, id).isValid())
        return makeInfo (*song, id);

    return {};
}

ModulatorID addModulator (ProjectContext& context, TrackID id, ModulatorType type)
{
    auto* song = findSong (context);

    if (song == nullptr || ! id.isValid() || ! findTrackState (*song, id).isValid())
        return {};

    const UndoStep step (*song, "Add " + getModulatorTypeName (type));
    return addModulator (*song, id, type);
}

void removeModulator (ProjectContext& context, ModulatorID id)
{
    // A container keeps its eight macros.
    if (auto* song = findSong (context);
        song != nullptr && getModulatorTrack (*song, id).isValid() && ! getModulatorContainer (*song, id).isValid())
    {
        const UndoStep step (*song, "Remove " + getModulatorName (*song, id));
        removeModulator (*song, id);
    }
}

bool isModulatorOn (const ProjectContext& context, ModulatorID id)
{
    auto* song = findSong (context);
    return song != nullptr && isModulatorOn (*song, id);
}

void setModulatorOn (ProjectContext& context, ModulatorID id, bool on)
{
    if (auto* song = findSong (context);
        song != nullptr && findModulatorState (*song, id).hasType (schema::MODULATOR) && isModulatorOn (*song, id) != on)
    {
        const UndoStep step (*song, on ? "Turn Modulator On" : "Turn Modulator Off");
        setModulatorOn (*song, id, on);
    }
}

std::vector<ModulatorParameter> getModulatorParameters (const ProjectContext& context, ModulatorID id)
{
    std::vector<ModulatorParameter> parameters;
    auto* song = findSong (context);
    const auto state = song != nullptr ? findModulatorState (*song, id) : juce::ValueTree();

    // A modulator's as its kind describes them (its switch is not among them, but the modulator's own); a macro's value.
    if (const auto kind = getModulatorKind (state))
        for (const auto& info : devices::getModulatorParameters (*kind))
            parameters.push_back ({ toString (info.id), toString (info.name) });

    if (state.hasType (schema::MACRO))
        parameters.push_back ({ getMacroParameterID (id), getModulatorName (*song, id) });

    return parameters;
}

ParameterHandle getModulatorParameter (ProjectContext& context, ModulatorID id, const juce::String& parameterID)
{
    auto* song = findSong (context);
    const auto state = song != nullptr ? findModulatorState (*song, id) : juce::ValueTree();

    if (const auto kind = getModulatorKind (state))
    {
        const auto index = devices::findModulatorParameter (*kind, parameterID.toStdString());

        if (index < 0)
            return {};

        const auto& info = devices::getModulatorParameters (*kind)[(size_t) index];
        return makeParameterHandle (*song, getRef (id, parameterID), "Change " + toString (info.name), false);
    }

    if (! state.hasType (schema::MACRO) || parameterID != getMacroParameterID (id))
        return {};

    return makeParameterHandle (*song, getRef (id, parameterID), "Change " + getModulatorName (*song, id), false);
}

void renameMacro (ProjectContext& context, ModulatorID id, const juce::String& name)
{
    auto* song = findSong (context);
    const auto state = song != nullptr ? findModulatorState (*song, id) : juce::ValueTree();

    if (! state.hasType (schema::MACRO) || name.trim().isEmpty() || name.trim() == state[schema::macroName].toString())
        return;

    const UndoStep step (*song, "Rename Macro");
    renameMacro (*song, id, name);
}

std::vector<float> getModulatorOutput (const ProjectContext& context, ModulatorID id, double seconds)
{
    // As Phazite's engine played it, while it plays the song.
    if (auto* song = findSong (context))
        if (auto* node = findPlayedModulator (*song, id))
            return node->getHistory (seconds);

    return {};
}

float getModulatorValue (const ProjectContext& context, ModulatorID id)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return 0.0f;

    if (auto* node = findPlayedModulator (*song, id))
        return node->isMacro() ? node->getValue() : juce::jlimit (-1.0f, 1.0f, node->getValue());

    // Without the engine, a macro's value is the one set by hand, and a modulator moves nothing.
    if (const auto state = findModulatorState (*song, id); state.hasType (schema::MACRO))
        return (float) state.getProperty (schema::macroValue, 0.0f);

    return 0.0f;
}

bool isBipolar (const ProjectContext& context, ModulatorID id)
{
    auto* song = findSong (context);
    return song != nullptr && isBipolar (*song, id);
}

std::vector<float> getSteps (const ProjectContext& context, ModulatorID id)
{
    std::vector<float> steps;
    auto* song = findSong (context);

    if (const auto sequencer = song != nullptr ? findSequencer (*song, id) : juce::ValueTree(); sequencer.isValid())
    {
        steps = getModulatorSteps (sequencer);
        steps.resize ((size_t) getNumSteps (sequencer), 0.0f);
    }

    return steps;
}

void setStep (ProjectContext& context, ModulatorID id, int index, float value)
{
    auto* song = findSong (context);

    if (const auto sequencer = song != nullptr ? findSequencer (*song, id) : juce::ValueTree();
        sequencer.isValid() && juce::isPositiveAndBelow (index, getNumSteps (sequencer)))
    {
        const UndoStep step (*song, "Change Step");
        setModulatorStep (*song, id, index, value);
    }
}

//==============================================================================
std::vector<Modulation> getModulations (const ProjectContext& context, const ParameterRef& ref)
{
    auto* song = findSong (context);
    return song != nullptr ? getModulations (*song, ref) : std::vector<Modulation>();
}

std::vector<Modulation> getModulations (const ProjectContext& context, ModulatorID id)
{
    auto* song = findSong (context);
    return song != nullptr ? getModulations (*song, id) : std::vector<Modulation>();
}

bool canModulate (const ProjectContext& context, ModulatorID id, const ParameterRef& ref)
{
    auto* song = findSong (context);
    return song != nullptr && canModulate (*song, id, ref);
}

void modulate (ProjectContext& context, ModulatorID id, const ParameterRef& ref, float amount)
{
    if (auto* song = findSong (context); song != nullptr && canModulate (*song, id, ref))
        modulateAsStep (*song, id, ref, amount);
}

void removeModulation (ProjectContext& context, ModulatorID id, const ParameterRef& ref)
{
    if (auto* song = findSong (context))
        removeAsStep (*song, id, ref);
}

//==============================================================================
std::vector<ModulatorInfo> getModulatorsFor (const ParameterHandle& handle)
{
    std::vector<ModulatorInfo> modulators;
    const auto target = findTarget (handle);

    if (! target.has_value())
        return modulators;

    const auto& [song, ref] = *target;

    // A device's in a container, or a container's chain's: its container's macros first.
    auto container = findContainerOf (*song, ref.device);

    if (! container.isValid())
        container = getChainContainer (*song, ref.device);

    if (container.isValid())
        for (const auto macro : getContainerMacros (*song, container))
            modulators.push_back (makeInfo (*song, macro));

    if (const auto track = getParameterTrack (*song, ref); track.isValid())
        for (const auto modulator : getModulators (*song, track))
            modulators.push_back (makeInfo (*song, modulator));

    return modulators;
}

std::vector<Modulation> getModulations (const ParameterHandle& handle)
{
    const auto target = findTarget (handle);
    return target.has_value() ? getModulations (*target->first, target->second) : std::vector<Modulation>();
}

ModulatorID modulateWithNew (const ParameterHandle& handle, ModulatorType type, float amount)
{
    const auto target = findTarget (handle);
    const auto track = target.has_value() ? getParameterTrack (*target->first, target->second) : TrackID();

    if (! track.isValid())
        return {};

    auto& song = *target->first;
    const auto& ref = target->second;
    const UndoStep step (song, "Modulate " + getParameterName (song, ref));
    const auto added = addModulator (song, track, type);

    if (! added.isValid() || ! canModulate (song, added, ref))
        return {};

    setModulation (song, added, ref, amount);
    return added;
}

void modulate (const ParameterHandle& handle, ModulatorID id, float amount)
{
    if (const auto target = findTarget (handle); target.has_value() && canModulate (*target->first, id, target->second))
        modulateAsStep (*target->first, id, target->second, amount);
}

void removeModulation (const ParameterHandle& handle, ModulatorID id)
{
    if (const auto target = findTarget (handle))
        removeAsStep (*target->first, id, target->second);
}

//==============================================================================
struct ModulationDrag::Impl
{
    Impl (Song& s, ModulatorID m, const ParameterRef& p, float amount)
        : song (&s),
          modulator (m),
          parameter (p),
          startAmount (amount),
          gesture (s, "Change Modulation")
    {
    }

    Song::Ptr song; // kept while the drag lasts, as its gesture's
    ModulatorID modulator;
    ParameterRef parameter;
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
    setModulation (*impl->song, impl->modulator, impl->parameter, amount);
}

std::unique_ptr<ModulationDrag> beginModulationDrag (const ParameterHandle& handle, ModulatorID id)
{
    const auto target = findTarget (handle);

    if (! target.has_value())
        return {};

    for (const auto& modulation : getModulations (*target->first, target->second))
        if (modulation.modulator == id)
            return std::make_unique<ModulationDrag> (
                std::make_unique<ModulationDrag::Impl> (*target->first, id, target->second, modulation.amount));

    return {};
}

std::optional<ModulationRange> getModulationRange (const ParameterHandle& handle)
{
    const auto target = findTarget (handle);

    if (! target.has_value() || ! isModulated (*target->first, target->second))
        return {};

    const auto& song = *target->first;
    const auto found = locateParameter (song, target->second);
    const auto modulations = getModulations (song, target->second);

    if (! found.has_value() || modulations.empty())
        return {};

    // From where its value is without modulation (its automation's, while the engine plays it), each modulation's reach: a
    // bipolar modulator's both ways. Only those whose modulators are on, unless all are off.
    const auto anyOn =
        std::any_of (modulations.begin(), modulations.end(), [&song] (const Modulation& m) { return isModulatorOn (song, m.modulator); });
    const auto range = describeParameter (song, *found).range;
    const auto toPart = [&range] (float value) { return range.convertTo0to1 (range.snapToLegalValue (value)); };
    const auto base = toPart (getCurrentValue (song, *found, false));
    auto low = base, high = base;

    for (const auto& modulation : modulations)
    {
        if (anyOn && ! isModulatorOn (song, modulation.modulator))
            continue;

        const auto amount = modulation.amount;
        const auto reach = isBipolar (song, modulation.modulator) ? std::abs (amount) : 0.0f;
        low += std::min ({ amount, -reach, 0.0f });
        high += std::max ({ amount, reach, 0.0f });
    }

    return ModulationRange { juce::jlimit (0.0f, 1.0f, low), juce::jlimit (0.0f, 1.0f, high), toPart (getCurrentValue (song, *found, true)),
                             anyOn };
}
} // namespace phazite
