// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's editor (roadmap steps 1.6 and 4.12, devices/drums/DrumSampler.h). Its Pads: sixteen of its 128 at a
// time, in four rows of four, the lowest at the bottom left as on FL Studio's FPC and an MPC, beside a map of all of them,
// which shows the pads that play a sound, lights each as it plays and frames the sixteen shown; a click or a drag on the map,
// or the mouse wheel over the pads, shows others a row at a time; at first, the default kit's sixteen, from the kick (note
// 36). Each pad shows its note, what it plays (an empty one, faintly, the drum General MIDI puts on its note), its choke
// group in its corner, M while it is muted (faint) and S while it is soloed, and lights when it plays. Clicking a pad, or
// Space or Enter on it, plays it and shows it: its Sound, drawn as it plays (reversed, from its end), with its envelope over
// it: its start, the attack's ramp, the hold and the decay, each a handle dragged sideways as one undo step (a press takes
// the nearest, which lights up under the pointer); and a line
// where it is as it plays, with Reverse, Swap (model/devices/SwapModel.h) and Reset beside its name and its start, attack,
// hold, decay and mode under it. Its Layers: how its sounds share its hits, and each layer's sound, which a sound dropped
// on it replaces and its Clear takes off, with its level. Then the pad's own level, pan, velocity, choke group, mute and solo, its
// Pitch (its tuning and its sweep), its Filter, and the sampler's Output, which every pad goes through but one on a track
// of its own, with Own Track, which gives the selected pad one or brings it back. A sound dropped on a pad from the
// browser or Explorer plays there. A dock too narrow for all of it shows the rest in pages, the pads on each
// (DeviceLayout.h).
#pragma once

#include "DeviceEditorBase.h"
#include "devices/drums/DrumSampler.h"
#include "ui/components/DisplayTicker.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>
#include <vector>

namespace phazite::ui
{
class DrumSamplerEditor : public DeviceEditorBase,
                          private DisplayTicker::Display
{
public:
    explicit DrumSamplerEditor (const DeviceHandle&);
    ~DrumSamplerEditor() override;

    // The pad whose controls show, by its note: the one played or dropped on last; at first the kit's kick.
    int getSelectedPad() const noexcept { return selectedPad; }
    void selectPad (int pad);

    // The lowest of the sixteen pads shown, a multiple of four; and showing others, from the row a pad is in.
    int getFirstShownPad() const noexcept { return firstShown; }
    void showPadsFrom (int pad);

    // A pad's button, by its note, while it shows (nullptr otherwise); and what dropping a file on a pad, or on one of its
    // layers, does, as a drop from the browser or Explorer.
    juce::Button* getPad (int pad);
    bool dropFile (int pad, const juce::File&, int layer = 0);

    // The selected pad's layer's place in the Layers part, which a sound is dropped on, showing what it plays.
    juce::Component& getLayerSlot (int layer) const;

    // The map of all the pads, beside the sixteen shown.
    juce::Component& getPadMap() const;

    // Swap, beside Reset while what shows the editor sets onSwap (the browser, through the Devices view or the device's
    // window): asked for the selected pad. Lit while that pad is being swapped, which a click on it then keeps.
    std::function<void (int pad)> onSwap;
    juce::Button& getSwapButton() noexcept;

    // Own Track, under the sampler's output: lit while the selected pad plays on a mixer channel of its own (a track after the
    // sampler's), which a click gives it, or takes away, bringing the pad back to the sampler's track, each as one undo step.
    juce::Button& getOwnTrackButton() noexcept;

    // The selected pad's sound, drawn; and where it draws the handle of its start, of its attack's end, of its hold's end
    // or of its decay's end (one of those fields), in its own coordinates.
    juce::Component& getWaveform() const;
    float getHandleX (devices::DrumSampler::Field) const;

    // Brings the pads, the map, the drawing and the buttons up to date; the ticker does it 30 times a second.
    void refresh();

private:
    class Pad;
    class PadMap;
    class PadsPart;
    class Waveform;
    class SoundPart;
    class LayerSlot;
    class LayersPart;
    class OutputPart;

    int selectedPad = devices::DrumSampler::kitFirstNote;
    int firstShown = devices::DrumSampler::kitFirstNote;
    int swappedPad = -1; // the pad being swapped, as last shown, or -1
    PadsPart* padsPart = nullptr;
    SoundPart* soundPart = nullptr;
    LayersPart* layersPart = nullptr;
    OutputPart* outputPart = nullptr;

    // The controls of the selected pad's parameters, each with the fields they show, in their order.
    std::vector<std::pair<ParameterControls*, std::vector<devices::DrumSampler::Field>>> padControls;

    ParameterControls& makePadControls (std::vector<devices::DrumSampler::Field>, int columns, const std::vector<juce::String>& names);
    DeviceLayout::Part makePadPart (const juce::String& title, std::vector<devices::DrumSampler::Field>, int columns,
                                    const std::vector<juce::String>& names);
    void play (int pad);
    void tick() override { refresh(); }
};
} // namespace phazite::ui
