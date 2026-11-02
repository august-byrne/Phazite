// SPDX-License-Identifier: AGPL-3.0-or-later
// What modulates one of Phazite's devices, as its plug-in plays it (roadmap A5, part 4; docs/design.md, section 10): each
// modulator or macro that moves one of its parameters, and by how much. The message thread reads it from Tracktion's
// modifiers, macros and assignments (model/modulation/internal/Modulation.h: updateDeviceModulation()) and hands it to
// the audio thread whole (DevicePlugin::setModulation()). The plug-in then sets each source's values for a block from
// Tracktion's modulators, and the device's runner moves each such parameter itself, in every stretch of a block, from its
// value as automation leaves it (engine/nodes/DeviceRunner.h): by an LFO's own wave (devices/modulation/Modulation.h)
// from where Tracktion's LFO is as the block starts, and by any other modulator or macro gliding from one block's value
// to the next. Tracktion still moves plug-ins' parameters and the mixer's, once a block.
#pragma once

#include "engine/nodes/DeviceRunner.h"
#include "model/base/internal/Tracktion.h"

#include <vector>

namespace phazite
{
struct DeviceModulation
{
    // What each of the plan's sources is in Tracktion: a modulator, or a macro.
    struct Source
    {
        te::Modifier::Ptr modifier;
        te::MacroParameter::Ptr macro;
        te::LFOModifier* lfo = nullptr; // the modulator, if it is an LFO
    };

    std::vector<Source> sources;
    engine::Modulation plan; // what the runner plays: its sources in the same order, and what they move
};
} // namespace phazite
