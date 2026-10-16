// SPDX-License-Identifier: AGPL-3.0-or-later
// Making ParameterHandles (model/devices/ParameterHandle.h) of a plug-in's parameters, for the model, the app and tests.
// Not for the UI, which gets handles from the model's functions by IDs.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
class ParameterHandle;

// A plug-in's parameter by its ID; changes to it are undo steps named undoName ("Change Volume"), or "Change" and the
// parameter's name. A handle of nothing if the plug-in is nullptr.
ParameterHandle makeParameterHandle (te::Plugin*, const juce::String& parameterID, const juce::String& undoName = {});
} // namespace phazite
