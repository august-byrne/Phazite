// SPDX-License-Identifier: AGPL-3.0-or-later
// The reverb's editor (devices/reverb/Reverb.h), as FabFilter's Pro-R shows its decay: a map of the tail, time across and
// pitch up, each pitch lit until it has faded by 60 dB: long in the bass, shorter in the treble past the damping, after the
// pre-delay's gap, and nothing below the low cut, so its slanted wedge says how the space sounds. Hits drift into it and fade
// as they play. Its edges drag, each one undo step, from where they were taken (graph::HandleDrag; Escape takes a drag back):
// the wedge's end for the decay, the gap for the pre-delay, the slant's corner for the damping and the floor for the low cut;
// the one under the pointer lights up, and the pointer says which way it moves. Its top names the mode, a room or a plate, and what a plate holds
// back of its Diffusion and Size where the Decay is too short for them. Then its Decay and Size, large, its Mode with how
// the sound goes in, and its tone.
#pragma once

#include "DeviceEditorBase.h"

#include <cstdint>

namespace phazite::ui
{
class ReverbEditor : public DeviceEditorBase
{
public:
    explicit ReverbEditor (const DeviceHandle&);
    ~ReverbEditor() override;

    juce::Component& getGraph() const noexcept;

    // The times the map shows, in seconds: up to where the slowest fade ends, and a little after.
    double getShownSeconds() const;

    // Where the map draws the tail's end for the bass (the decay), the pre-delay's edge and the damping's corner, in its own
    // coordinates.
    juce::Point<float> getDecayHandle() const;
    juce::Point<float> getPredelayHandle() const;
    juce::Point<float> getDampingHandle() const;

    // What the map's top says of the mode: its name, and for a plate what it holds back ("Plate  Diffusion 58%").
    juce::String getModeText() const;

    // The work the map's decays have taken since it was made, in the reverb's lines' energies worked out
    // (Reverb::getDecayAt()): it works them out again when a value or the sample rate changes, not for each paint. For tests.
    std::int64_t getDecayLineUpdates() const;

    // Brings the map up to date; the ticker does it.
    void refresh();

    // How many times the map has worked the tail's shape out: once for each change of the values it shows, not at each repaint.
    int getShapesWorkedOut() const noexcept;

private:
    class Graph;
    Graph* graph = nullptr;
};
} // namespace phazite::ui
