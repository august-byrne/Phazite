// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/Commands.h"
#include "model/ComputerKeyboard.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
// The bar along the top: transport, tempo, key, CPU, the computer keyboard and the command search.
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

    juce::TextButton playButton { "Play" }, keyboardButton, searchButton;
    juce::Label tempoLabel, keyLabel, cpuLabel;
    int shownKeyboardState = -1; // what the keyboard button shows, to update it only on changes

    void timerCallback() override; // updates the readouts a few times a second; only changes repaint
    void updateKeyboardButton();
    void setTooltipWithShortcut (juce::SettableTooltipClient&, const juce::String& text, const juce::String& commandID);
};
} // namespace phazite::ui
