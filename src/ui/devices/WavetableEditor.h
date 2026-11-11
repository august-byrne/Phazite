// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth's editor (devices/wavetable/WavetableSynth.h; roadmap step 2.13), a part to a column as the sound
// goes through them. First its oscillators, each showing its table as its frames one behind another, as Serum and Ableton
// Live's Wavetable draw theirs, the frame it plays lit as the newest note moves through the table; above, a button with the
// table's name opens a menu of the factory tables and Load Wavetable File..., a sound dropped on the drawing (from the
// browser or Explorer) becomes the table, and a drag across it moves the oscillator's position (the grabbing hand over it,
// the frame it plays ringed). Then its filter's response,
// which follows the newest note, with the cutoff a dot to drag, and the amp's envelope; then the sub and noise over the
// unison, and how it plays. Last, its other envelopes and LFOs, each drawn with how far the newest note has got, and the
// modulation matrix, in a list down the side. A dock narrower than them shows them in pages.
#pragma once

#include "DeviceEditorBase.h"
#include "SynthParts.h"
#include "devices/wavetable/Wavetable.h"

#include <array>
#include <memory>

namespace phazite::ui
{
class WavetableEditor : public DeviceEditorBase,
                        private DisplayTicker::Display
{
public:
    static constexpr int numOscillators = 2;

    explicit WavetableEditor (const DeviceHandle&);
    ~WavetableEditor() override;

    // An oscillator's drawing of its table.
    juce::Component& getTableView (int osc) const;

    // The button that names an oscillator's table, and the menu it opens: the factory tables, the one playing ticked, then
    // Load Wavetable File....
    juce::Button& getTableButton (int osc) const;
    juce::PopupMenu getTableMenu (int osc);

    // Puts a sound file on an oscillator as its table; returns whether it did.
    bool dropFile (int osc, const juce::File&);

    // Shows what changed elsewhere (undo, the table, where it plays); the ticker does it.
    void refresh();

private:
    class TableView;
    class Oscillator;
    class Matrix;

    std::array<TableView*, numOscillators> views {};
    std::array<juce::TextButton*, numOscillators> tableButtons {};
    std::unique_ptr<juce::FileChooser> chooser;

    void chooseFile (int osc);
    void tick() override { refresh(); }
};
} // namespace phazite::ui
