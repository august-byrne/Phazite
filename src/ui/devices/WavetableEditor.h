// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth's editor (devices/wavetable/WavetableSynth.h; roadmap step 2.13). Each oscillator shows its table as
// its frames one behind another, as Serum and Ableton Live's Wavetable draw theirs, with the frame it plays in the accent
// color as the newest note moves through the table; above, a button with the table's name opens a menu of the factory
// tables and Load Wavetable File..., and a sound dropped on the drawing, from the browser or Explorer, becomes the table.
// Dragging across the drawing moves the oscillator's position. Under each drawing are its oscillator's controls; below them
// the rest of the synth's, in groups in the order the sound goes through them (DevicePanel.h), the modulation slots last.
#pragma once

#include "DeviceEditors.h"
#include "DevicePanel.h"
#include "devices/wavetable/Wavetable.h"
#include "model/devices/DeviceHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace phazite::ui
{
class WavetableEditor : public juce::Component,
                        public FlexibleHeight,
                        private juce::Timer
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

    // Shows what changed elsewhere (undo, the table, where it plays); a timer does it 15 times a second.
    void refresh();

    DevicePanel& getOscillatorControls (int osc) const;
    DevicePanel& getControls() const noexcept { return *controls; }

    void paint (juce::Graphics&) override;
    void resized() override;

    int getMinimumHeight() const override;
    int getWidthForHeight (int height) const override;

private:
    class TableView;

    DeviceHandle device;
    std::array<std::unique_ptr<TableView>, numOscillators> views;
    std::array<std::unique_ptr<juce::TextButton>, numOscillators> tableButtons;
    std::array<std::unique_ptr<DevicePanel>, numOscillators> oscillatorControls;
    std::unique_ptr<DevicePanel> controls; // the sub, noise, unison, filter, envelopes, LFOs, voices and modulation
    std::unique_ptr<juce::FileChooser> chooser;

    int getColumnWidth (int osc) const;
    int getTopHeight (int drawingHeight) const; // the oscillators' columns
    void chooseFile (int osc);
    void timerCallback() override;
};
} // namespace phazite::ui
