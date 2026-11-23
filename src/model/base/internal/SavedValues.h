// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter's value set by hand as the song keeps it (Schema.h: VALUES, a device's and a modulator's): whether it is the
// parameter's default, which the song keeps none of, and its saved form, a choice by its name. Plain functions of the
// parameter's description alone (devices/core/DeviceInfo.h), in the model's base, so that the conversion of the song's
// values (ModulationValue.h: a modulator's) and the devices' code (model/devices/internal/Parameters.h, which includes
// this) read and write them alike. Any thread.
#pragma once

#include "devices/core/DeviceInfo.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace phazite
{
// Whether a value is its parameter's default, to a millionth of the default's size (a value back from a control's range
// can be a little off): what the song keeps none of.
bool isDefaultValue (const devices::ParameterInfo&, float value);

// A value set by hand as the song and presets keep it: a choice by its name (devices::getChoiceName(): "Low Pass"), so that
// what it means doesn't hang on the choices' order and another tool can read it, any other as its number. And back: a
// choice by its name, a value by its number, and the parameter's default for none. Every reader of VALUES goes through it.
juce::var toSavedValue (const devices::ParameterInfo&, float value);
float readSavedValue (const devices::ParameterInfo&, const juce::var& saved);

// A value set by hand as the song holds it, from its saved form: a choice by a name it has, or by its number where it is held
// as a number (not as text), any other by its number (SongText.h: readValueNumber()), brought within its range and to its
// steps (devices::constrain()); none for none, for what is no finite number, for a choice written as text that is none of its
// names (its number's text, "2"), which the loader leaves none of (model/project/internal/Project.h: makeConsistent()), and
// for one at its default, which the song keeps none of. What the song's values read a device's and a modulator's values with
// as the song holds them (DevicesValue.h: readHeldDevice(); ModulationValue.h: readHeldModulator()).
std::optional<float> readHeldSavedValue (const devices::ParameterInfo&, const juce::var& saved);
} // namespace phazite
