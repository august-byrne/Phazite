// SPDX-License-Identifier: AGPL-3.0-or-later
// What modulates one of Phazite's devices, as its plug-in plays it (roadmap A5, part 4; docs/design.md, section 10): each
// modulator or macro that moves one of its parameters, and by how much. The message thread reads it from Tracktion's
// modifiers, macros and assignments (model/modulation/internal/Modulation.h: updateDeviceModulation()) and hands it to
// the audio thread whole (DevicePlugin::setModulation()). The plug-in then moves each such parameter itself, in every
// stretch of a block, from its value as automation leaves it: by an LFO's own wave (devices/modulation/Modulation.h)
// from where Tracktion's LFO is as the block starts, and by any other modulator or macro gliding from one block's value
// to the next. Tracktion still moves plug-ins' parameters and the mixer's, once a block.
#pragma once

#include "devices/modulation/Modulation.h"
#include "model/base/internal/Tracktion.h"

#include <vector>

namespace phazite
{
struct DeviceModulation
{
    // What moves parameters: a modulator, or a macro.
    struct Source
    {
        te::Modifier::Ptr modifier;
        te::MacroParameter::Ptr macro;
        te::LFOModifier* lfo = nullptr; // the modulator, if it is an LFO

        // The audio thread's, for the block it plays: whether it is on, its value as the last block started and as this
        // one does, and an LFO's wave from where its cycle is at the block's start.
        bool isOn = true, started = false, followsWave = false;
        float previous = 0.0f, now = 0.0f;
        devices::LfoShape shape;
        float phase = 0.0f;
        double cyclesPerSample = 0.0;
    };

    // A source moving a parameter, by its index among the device's, by an amount from -1 to 1.
    struct Route
    {
        int parameter = 0;
        int source = 0;
        float amount = 0.0f;
    };

    std::vector<Source> sources;
    std::vector<Route> routes;
    std::vector<bool> isModulated; // by the parameter's index
};
} // namespace phazite
