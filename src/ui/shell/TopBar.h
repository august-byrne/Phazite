// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite
{
class Gesture;
class ProjectContext;
} // namespace phazite

namespace phazite::ui
{
// The bar along the top: play, loop, the metronome, the tempo and time signature, CPU, the master level and volume,
// the computer keyboard and the command search. (A key is a track's, so the piano roll shows it.)
class TopBar : public juce::Component,
               private juce::Timer
{
public:
    TopBar (te::Engine&, CommandBridge&, ComputerKeyboard&, const ProjectContext&);
    ~TopBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override; // the record button is red while recording

    juce::Component& getSearchButton() noexcept { return searchButton; } // the palette opens under it

    // Puts the current shortcuts into the tooltips; call after adding commands or changing keymap.
    void refreshTooltips();

private:
    te::Engine& engine;
    CommandBridge& commands;
    ComputerKeyboard& computerKeyboard;
    const ProjectContext& projectContext;

    // A transport button whose right-click opens its options, rather than clicking it.
    struct OptionsButton : public IconButton
    {
        using IconButton::clicked;
        using IconButton::IconButton;
        std::function<void (bool isMenuClick)> onClicked;

        void clicked (const juce::ModifierKeys& mods) override
        {
            if (onClicked != nullptr)
                onClicked (mods.isPopupMenu());
        }
    };

    IconButton playButton { "Play", Icon::play }, captureButton { "Capture", Icon::capture }, loopButton { "Loop", Icon::loop },
        keyboardButton { "Computer keyboard", Icon::keyboard }, searchButton { "Search commands", Icon::search };
    OptionsButton recordButton { "Record", Icon::record }, metronomeButton { "Metronome", Icon::metronome };
    NumberField tempoField;                      // dragged, scrolled or typed, a drag being one undo step
    std::unique_ptr<phazite::Gesture> tempoDrag; // while the tempo is dragged
    juce::TextButton timeSignatureButton;        // a menu of time signatures
    juce::Label cpuLabel;
    LevelMeter masterMeter { "Master level" };
    Fader masterVolume { Fader::Direction::horizontal, "Master volume", te::decibelsToVolumeFaderPosition (0.0f) };
    ParameterAttachment masterVolumeAttachment { masterVolume, "Change Master Volume" };
    int shownKeyboardState = -1; // what the keyboard button shows, to update it only on changes

    void timerCallback() override; // updates the readouts a few times a second; only changes repaint
    void updateKeyboardButton();
    void updateTempo();
    void showRecordMenu();
    void showMetronomeMenu();
    void showTimeSignatureMenu();
    bool isCommandTicked (const juce::String& commandID) const;
    bool isCommandEnabled (const juce::String& commandID) const;
};
} // namespace phazite::ui
