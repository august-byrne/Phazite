// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "LookAndFeel.h"

namespace phazite::ui
{
TopBar::TopBar (te::Engine& e, CommandRegistry& registry, ComputerKeyboard& keyboard, std::function<te::Edit*()> editGetter)
    : engine (e),
      commands (registry),
      computerKeyboard (keyboard),
      getEdit (std::move (editGetter))
{
    setTitle ("Transport");

    playButton.onClick = [this] { commands.invoke ("transport.playStop"); };
    keyboardButton.onClick = [this] { commands.invoke ("input.computerKeyboard"); };

    searchButton.setButtonText ("Search commands");
    searchButton.onClick = [this] { commands.invoke ("view.commandPalette"); };
    refreshTooltips();

    for (auto* label : { &tempoLabel, &keyLabel, &cpuLabel })
    {
        label->setFont (LookAndFeel::getNumberFont (TextSize::body));
        label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*label);
    }

    tempoLabel.setTooltip ("Tempo, in beats per minute");
    keyLabel.setTooltip ("The project's key");
    cpuLabel.setTooltip ("How much of the time available for each audio block the engine uses");

    addAndMakeVisible (playButton);
    addAndMakeVisible (keyboardButton);
    addAndMakeVisible (searchButton);

    timerCallback();
    startTimerHz (4);
}

void TopBar::refreshTooltips()
{
    setTooltipWithShortcut (playButton, "Play or stop", "transport.playStop");
    setTooltipWithShortcut (searchButton, "Find and run any command by typing part of its name", "view.commandPalette");

    shownKeyboardState = -1; // its tooltip names a shortcut too
    updateKeyboardButton();
}

void TopBar::updateKeyboardButton()
{
    const bool on = computerKeyboard.isEnabled();
    const auto lowestNote = computerKeyboard.getLowestNote(), velocity = computerKeyboard.getVelocity();
    const auto state = (on ? 1 : 0) + 2 * lowestNote + 256 * velocity; // lowestNote < 128

    if (state == shownKeyboardState)
        return;

    shownKeyboardState = state;
    const auto from = juce::MidiMessage::getMidiNoteName (lowestNote, true, true, 4); // C4 is middle C

    keyboardButton.setToggleState (on, juce::dontSendNotification);
    keyboardButton.setButtonText (on ? "Keys " + from : juce::String ("Keys off"));
    setTooltipWithShortcut (keyboardButton,
                            on ? "The computer keyboard plays notes: A S D F G H J K L ; from " + from
                                     + ", and W E T Y U O P the black keys. Z and X change the octave, C and V the velocity (now "
                                     + juce::String (velocity) + "). Click to turn it off"
                               : juce::String ("Click to play notes with the computer keyboard"),
                            "input.computerKeyboard");
}

void TopBar::setTooltipWithShortcut (juce::SettableTooltipClient& client, const juce::String& text, const juce::String& commandID)
{
    const auto shortcuts = commands.getShortcutDescriptions (commandID);
    client.setTooltip (shortcuts.isEmpty() ? text : text + " (" + shortcuts[0] + ")");
}

void TopBar::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

void TopBar::resized()
{
    auto area = getLocalBounds().reduced (Space::s, (getHeight() - controlHeight) / 2);

    playButton.setBounds (area.removeFromLeft (80));
    area.removeFromLeft (Space::m);
    tempoLabel.setBounds (area.removeFromLeft (90));
    keyLabel.setBounds (area.removeFromLeft (110));
    cpuLabel.setBounds (area.removeFromLeft (80));
    searchButton.setBounds (area.removeFromRight (220));
    area.removeFromRight (Space::s);
    keyboardButton.setBounds (area.removeFromRight (96));
}

void TopBar::timerCallback()
{
    auto* edit = getEdit();
    const bool playing = edit != nullptr && edit->getTransport().isPlaying();

    if (const auto text = juce::String (playing ? "Stop" : "Play"); playButton.getButtonText() != text)
        playButton.setButtonText (text);

    playButton.setEnabled (edit != nullptr);

    juce::String tempo, key;

    if (edit != nullptr)
    {
        const auto position = edit->getTransport().getPosition();
        tempo = juce::String (edit->tempoSequence.getBpmAt (position), 1) + " BPM";

        const auto& pitch = edit->pitchSequence.getPitchAt (position);
        key = juce::MidiMessage::getMidiNoteName (pitch.getPitch(), true, false, 0) + " "
              + te::Scale::getNameForType (pitch.getScale()).toLowerCase();
    }

    tempoLabel.setText (tempo, juce::dontSendNotification);
    keyLabel.setText (key, juce::dontSendNotification);
    cpuLabel.setText ("CPU " + juce::String (juce::roundToInt (engine.getDeviceManager().getCpuUsage() * 100.0)) + "%",
                      juce::dontSendNotification);
    updateKeyboardButton();
}
} // namespace phazite::ui
