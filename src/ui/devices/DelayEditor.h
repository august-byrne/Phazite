// SPDX-License-Identifier: AGPL-3.0-or-later
// The delay's editor (devices/delay/Delay.h), as FabFilter's Timeless shows its echoes: the sound coming in as a bar, then
// each echo a bar after it, on the song's beats (or in milliseconds), fading as the feedback takes it down, above and below
// the middle in turn when it bounces from side to side. What plays lights the bar coming in, then each echo in its turn. A
// drag puts the first echo under the pointer, sideways for the time (a note value while it follows the tempo) and up or
// down for the feedback, as one undo step. Under them, the band its echoes keep, between the low and the high cut, whose
// ends drag. Then the time's controls and the feedback, large.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class DelayEditor : public DeviceEditorBase
{
public:
    explicit DelayEditor (const DeviceHandle&);
    ~DelayEditor() override;

    juce::Component& getEchoes() const noexcept;

    // Where the echoes' display draws its echo number n (0 for the sound coming in), in its own coordinates, and the band
    // its echoes keep.
    juce::Point<float> getEchoPosition (int n) const;
    juce::Rectangle<int> getBandArea() const;

private:
    class Echoes;
    Echoes* echoes = nullptr;
};
} // namespace phazite::ui
