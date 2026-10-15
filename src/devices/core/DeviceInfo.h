// SPDX-License-Identifier: AGPL-3.0-or-later
// What a device is and what its parameters are, without any engine (docs/design.md, section 24). The DSP of each of
// Phazite's devices describes itself with these: its thin Tracktion plug-in (src/model/devices/) turns the parameters
// into automatable ones, editors show them, and the DSP reads their current values. Type and parameter IDs are saved
// in projects, so never change one (model/base/Schema.h).
#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <span>
#include <string_view>

namespace phazite::devices
{
// What a parameter's value means, so that editors can show it ("-6.0 dB", "1.20 kHz", "35 %") and move it well.
enum class Unit
{
    none, // a plain number
    decibels,
    hertz,
    milliseconds,
    seconds,
    percent, // a fraction shown as a percentage: 0.35 is "35 %"
    semitones,
    cents,
    ratio,  // a compressor's ratio: 4 is "4:1"
    choice, // a whole number that picks one of the parameter's choices
    toggle  // 0 (off) or 1 (on)
};

struct ParameterInfo
{
    std::string_view id;   // saved in projects: never change it
    std::string_view name; // what editors call it, such as "Cutoff"
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    Unit unit = Unit::none;

    // For a skewed range, such as a frequency or a time: the value halfway along a knob's travel.
    std::optional<float> midpoint {};

    // For Unit::choice: what each whole number from the minimum up means.
    std::span<const std::string_view> choices {};
};

// Whether a description holds together: a range with the default in it, a midpoint strictly inside it, whole numbers
// for choices and toggles, and as many choices as the range has whole numbers.
bool isValid (const ParameterInfo&);

// The range a knob moves through: skewed so that the midpoint is halfway along, in whole steps for choices and
// toggles.
juce::NormalisableRange<float> getRange (const ParameterInfo&);

// A value as the parameter allows it: within its range, and whole for choices and toggles.
float constrain (const ParameterInfo&, float value);

//==============================================================================
enum class DeviceKind
{
    instrument, // turns notes into sound
    effect      // changes the sound that comes in
};

struct DeviceInfo
{
    std::string_view typeID; // saved in projects as the device's type: never change it
    std::string_view name;   // what menus and the mixer call it, such as "Synth"
    DeviceKind kind = DeviceKind::effect;
    std::span<const ParameterInfo> parameters {};
};

// The index of the parameter with that ID among the device's parameters, or -1.
int findParameter (const DeviceInfo&, std::string_view id);

// Whether the device has a type ID and a name, and parameters that are valid, with IDs that are unique and not empty.
bool isValid (const DeviceInfo&);
} // namespace phazite::devices
