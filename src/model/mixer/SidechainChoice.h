// SPDX-License-Identifier: AGPL-3.0-or-later
// A track a device's sidechain can listen to (internal/Sidechain.h), as the Devices view and the mixer show it, or none: the
// sound passing through the device. A track whose sound comes from the device's own, which would make a loop, says why it
// can't be picked.
#pragma once

#include "model/base/IDs.h"

#include <juce_graphics/juce_graphics.h>

namespace phazite
{
struct SidechainChoice
{
    TrackID track;
    juce::String name;
    juce::Colour colour {};
    juce::String whyNot {}; // empty for a track that can be picked
};
} // namespace phazite
