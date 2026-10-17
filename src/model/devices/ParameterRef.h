// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's parameter by IDs, as automation lanes, menus and the last-touched parameter name it outside the model: its
// device (a plug-in, one of Phazite's own devices, or a track's volume and pan, which are a device too) and the
// parameter's own ID ("volume", one of Phazite's devices' parameter IDs, or a plug-in parameter's number).
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

namespace phazite
{
struct ParameterRef
{
    DeviceID device {};
    juce::String parameter {};

    bool isValid() const noexcept { return device.isValid() && parameter.isNotEmpty(); }
    bool operator== (const ParameterRef&) const = default;
};
} // namespace phazite
