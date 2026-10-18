// SPDX-License-Identifier: AGPL-3.0-or-later
// Making ParameterHandles (model/devices/ParameterHandle.h) of a plug-in's parameters, for the model, the app and tests.
// Not for the UI, which gets handles from the model's functions by IDs.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <functional>

namespace phazite
{
class ParameterHandle;

// A plug-in's parameter by its ID; changes to it are undo steps named undoName ("Change Volume"), or "Change" and the
// parameter's name. A handle of nothing if the plug-in is nullptr.
ParameterHandle makeParameterHandle (te::Plugin*, const juce::String& parameterID, const juce::String& undoName = {});

// The same for a modulator's parameter (model/modulation/ModulationModel.h), and for a track's or a container's macro
// (model/mixer/internal/Containers.h: a container plays a rack type, which holds its macros).
ParameterHandle makeParameterHandle (te::Modifier&, const juce::String& parameterID, const juce::String& undoName = {});
ParameterHandle makeMacroHandle (te::AudioTrack&, const te::MacroParameter&, const juce::String& undoName = {});
ParameterHandle makeMacroHandle (te::RackType&, const te::MacroParameter&, const juce::String& undoName = {});

// A parameter of a device that a track may not hold yet, found each time with locate(), which the first change makes with
// create(), in that change's undo step, named createName (a drag's first move, or a value set outside a drag): a send to a
// return, which each strip offers before there is one (model/mixer/MixerModel.h: getSendLevelTo()). Until then its value
// is the start of the range given. Later changes are undo steps named undoName. identity tells it from handles of other
// devices that aren't there either.
ParameterHandle makeCreatingHandle (te::Track& owner, std::function<te::Plugin*(te::Track&)> locate,
                                    std::function<te::Plugin*(te::Track&)> create, const juce::String& identity,
                                    const juce::String& parameterID, juce::NormalisableRange<float>, const juce::String& createName,
                                    const juce::String& undoName);

// The parameter a handle changes, or nullptr for a handle of nothing or once its device is gone.
te::AutomatableParameter::Ptr findParameter (const ParameterHandle&);
} // namespace phazite
