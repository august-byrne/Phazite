// SPDX-License-Identifier: AGPL-3.0-or-later
// The settings window's content: the Audio, MIDI and Shortcuts tabs.
#pragma once

#include "model/commands/CommandRegistry.h"
#include "ui/input/ComputerKeyboard.h"

#include <juce_gui_extra/juce_gui_extra.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
//==============================================================================
// The MIDI tab: the MIDI input devices, each with an on/off switch, the computer keyboard, and the
// track that live MIDI plays.
class MidiSettings : public juce::Component,
                     private juce::ChangeListener
{
public:
    MidiSettings (te::Engine&, ComputerKeyboard&, std::function<te::Edit*()> getEdit);
    ~MidiSettings() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    te::Engine& engine;
    ComputerKeyboard& computerKeyboard;
    std::function<te::Edit*()> getEdit;

    juce::Label inputsHeading, noInputsLabel, keyboardHeading, keyboardHelp, routingLabel;
    juce::OwnedArray<juce::ToggleButton> inputButtons;
    juce::ToggleButton keyboardButton { "Play notes with the computer keyboard" };

    void rebuild();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuild(); } // devices changed
};

//==============================================================================
// The Shortcuts tab: the keymap, and every command's keys.
class ShortcutSettings : public juce::Component
{
public:
    ShortcutSettings (CommandRegistry&, std::function<void (const juce::String& keymapName)> chooseKeymap);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Label keymapLabel;
    juce::ComboBox keymapBox;
    juce::KeyMappingEditorComponent editor;
};

//==============================================================================
class SettingsPanel : public juce::TabbedComponent
{
public:
    enum Tab
    {
        audio,
        midi,
        shortcuts
    };

    SettingsPanel (te::Engine&, CommandRegistry&, ComputerKeyboard&, std::function<te::Edit*()> getEdit,
                   std::function<void (const juce::String& keymapName)> chooseKeymap);

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
};
} // namespace phazite::ui
