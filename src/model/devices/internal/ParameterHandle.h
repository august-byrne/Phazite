// SPDX-License-Identifier: AGPL-3.0-or-later
// Making ParameterHandles (model/devices/ParameterHandle.h) of a plug-in's parameters, for the model, the app and tests.
// Not for the UI, which gets handles from the model's functions by IDs.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class ParameterHandle;

// A plug-in's parameter by its ID; changes to it are undo steps named undoName ("Change Volume"), or "Change" and the
// parameter's name. A handle of nothing if the plug-in is nullptr.
ParameterHandle makeParameterHandle (te::Plugin*, const juce::String& parameterID, const juce::String& undoName = {});

// The same for a modulator's parameter (model/modulation/ModulationModel.h), and for a track's macro.
ParameterHandle makeParameterHandle (te::Modifier&, const juce::String& parameterID, const juce::String& undoName = {});
ParameterHandle makeMacroHandle (te::AudioTrack&, const te::MacroParameter&, const juce::String& undoName = {});

// The parameter a handle changes, or nullptr for a handle of nothing or once its device is gone.
te::AutomatableParameter::Ptr findParameter (const ParameterHandle&);
} // namespace phazite
