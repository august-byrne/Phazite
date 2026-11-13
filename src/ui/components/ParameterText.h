// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter's value as its automation lane, the inspector and the field typed in over an automation point show it, and a
// value typed for it read back: a fader's volume in decibels ("-6.0 dB"), a pan ("L20"), one of Phazite's devices' parameters
// in its units ("2.50 kHz", "40%"), and a plug-in's as the plug-in writes and reads it.
#pragma once

#include "model/automation/AutomationModel.h"
#include "model/devices/ParameterHandle.h"

#include <optional>

namespace phazite::ui
{
juce::String formatParameterText (const ParameterHandle&, ParameterKind, float value);

// The value a text typed for the parameter means, in the parameter's own terms (a fader's position, a pan from -1 to 1);
// nothing for text that names none.
std::optional<float> parseParameterText (const ParameterHandle&, ParameterKind, const juce::String&);
} // namespace phazite::ui
