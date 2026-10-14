// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/commands/CommandRegistry.h"
#include "ui/components/Controls.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
// The bar along the top: play, loop, tempo, CPU, the master level and volume, the computer keyboard and the
// command search. (A key is a track's, so the piano roll shows it.)
class TopBar : public juce::Component,
               private juce::Timer
{
public:
    TopBar (te::Engine&, CommandRegistry&, ComputerKeyboard&, std::function<te::Edit*()> getEdit);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Component& getSearchButton() noexcept { return searchButton; } // the palette opens under it

    // Puts the current shortcuts into the tooltips; call after adding commands or changing keymap.
    void refreshTooltips();

private:
    te::Engine& engine;
    CommandRegistry& commands;
    ComputerKeyboard& computerKeyboard;
    std::function<te::Edit*()> getEdit;

    IconButton playButton { "Play", Icon::play }, loopButton { "Loop", Icon::loop }, keyboardButton { "Computer keyboard", Icon::keyboard },
        searchButton { "Search commands", Icon::search };
    juce::Label tempoLabel, cpuLabel;
    LevelMeter masterMeter { "Master level" };
    Fader masterVolume { Fader::Direction::horizontal, "Master volume", te::decibelsToVolumeFaderPosition (0.0f) };
    ParameterAttachment masterVolumeAttachment { masterVolume, "Change Master Volume" };
    int shownKeyboardState = -1; // what the keyboard button shows, to update it only on changes

    void timerCallback() override; // updates the readouts a few times a second; only changes repaint
    void updateKeyboardButton();
};
} // namespace phazite::ui
