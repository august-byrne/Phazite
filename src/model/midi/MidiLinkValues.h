// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI learn's plain values (MidiLinkModel.h): a MIDI controller, and how it is named.
#pragma once

#include <juce_core/juce_core.h>

namespace phazite
{
// A MIDI controller (a control change), by its channel, 1 to 16, and its number, 0 to 119: 120 to 127 are channel mode
// messages, which no link takes.
struct MidiController
{
    static constexpr int numChannels = 16, numNumbers = 120;

    int channel = 1, number = 0;

    bool isValid() const noexcept { return channel >= 1 && channel <= numChannels && number >= 0 && number < numNumbers; }
    bool operator== (const MidiController&) const = default;
    auto operator<=> (const MidiController&) const = default;
};

// "CC 21, Channel 1", as the menus and the Project view name it; and "CC 21 on channel 1", as a sentence says it.
inline juce::String describeMidiController (MidiController controller)
{
    return "CC " + juce::String (controller.number) + ", Channel " + juce::String (controller.channel);
}

inline juce::String describeMidiControllerInText (MidiController controller)
{
    return "CC " + juce::String (controller.number) + " on channel " + juce::String (controller.channel);
}
} // namespace phazite
