// SPDX-License-Identifier: AGPL-3.0-or-later
// What a modulator dragged from its slot carries (ui/mixer/ModulatorSlots.h), for the knobs it is dropped on
// (Controls.h: Knob), which then modulate their parameters with it.
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace phazite::ui
{
inline juce::var makeModulatorDrag (ModulatorID modulator)
{
    return "phazite-modulator:" + juce::String ((juce::int64) modulator.getRaw());
}

inline std::optional<ModulatorID> readModulatorDrag (const juce::var& description)
{
    const auto text = description.toString();

    if (! text.startsWith ("phazite-modulator:"))
        return {};

    const auto raw = text.fromFirstOccurrenceOf (":", false, false).getLargeIntValue();
    return raw > 0 ? std::optional (ModulatorID::fromRaw ((std::uint64_t) raw)) : std::nullopt;
}
} // namespace phazite::ui
