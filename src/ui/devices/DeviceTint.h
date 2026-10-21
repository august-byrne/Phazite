// SPDX-License-Identifier: AGPL-3.0-or-later
// Each built-in device's colour, by its kind (DeviceFamily and its colours are design tokens: ui/theme/DesignTokens.h). An
// editor's knobs and graphs take it from the editor (applyDeviceTint()).
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/theme/DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string_view>

namespace phazite::ui
{
DeviceFamily getDeviceFamily (std::string_view typeID);

// Gives an editor its device's colour, which its knobs (their arcs) and graphs then take.
void applyDeviceTint (juce::Component& editor, const DeviceHandle&);

// The colour a component takes from its editor: its device's, or the accent.
juce::Colour getDeviceTint (const juce::Component&);
} // namespace phazite::ui
