// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulationValues.h"

namespace phazite
{
juce::String getModulatorTypeName (ModulatorType type)
{
    switch (type)
    {
        case ModulatorType::envelopeFollower:
            return "Envelope Follower";
        case ModulatorType::steps:
            return "Step Sequencer";
        case ModulatorType::random:
            return "Random";
        case ModulatorType::breakpoints:
            return "Breakpoint Envelope";
        case ModulatorType::noteFollower:
            return "Note Follower";
        case ModulatorType::macro:
            return "Macro";
        case ModulatorType::lfo:
        default:
            return "LFO";
    }
}
} // namespace phazite
