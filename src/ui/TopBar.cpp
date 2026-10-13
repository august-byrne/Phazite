// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "CommandUI.h"
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

    for (auto* label : { &tempoLabel, &cpuLabel })
    {
        label->setFont (LookAndFeel::getNumberFont (TextSize::body));
        label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*label);
    }

    tempoLabel.setTooltip ("Tempo, in beats per minute");
    cpuLabel.setTooltip ("How much of the time available for each audio block the engine uses");

    showAsDecibels (masterVolume);
    masterVolume.setTooltip ("The volume of everything Phazite plays. Double-click for 0 dB");

    // Clicking these buttons leaves the keyboard focus where it was; Tab still reaches them.
    for (auto* button : { &playButton, &keyboardButton, &searchButton })
    {
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }

    addAndMakeVisible (masterMeter);
    addAndMakeVisible (masterVolume);

    timerCallback();
    startTimerHz (4);
}

void TopBar::refreshTooltips()
{
    playButton.setTooltip (withShortcut (commands, "Play or stop", "transport.playStop"));
    searchButton.setTooltip (withShortcut (commands, "Find and run any command by typing part of its name", "view.commandPalette"));

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
    keyboardButton.setButtonText (on ? from : juce::String ("Off"));
    keyboardButton.setTooltip (withShortcut (commands,
                                             on ? "The computer keyboard plays notes: A S D F G H J K L ; from " + from
                                                      + ", and W E T Y U O P the black keys. Z and X change the octave, C and V the "
                                                        "velocity (now "
                                                      + juce::String (velocity)
                                                      + "). Hold Shift for single-key shortcuts. Click to turn it off"
                                                : juce::String ("Click to play notes with the computer keyboard"),
                                             "input.computerKeyboard"));
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

    playButton.setBounds (area.removeFromLeft (44));
    area.removeFromLeft (Space::m);

    searchButton.setBounds (area.removeFromRight (180));
    area.removeFromRight (Space::s);
    keyboardButton.setBounds (area.removeFromRight (76));
    area.removeFromRight (Space::m);
    masterVolume.setBounds (area.removeFromRight (120));
    area.removeFromRight (Space::xs);
    masterMeter.setBounds (area.removeFromRight (64).withSizeKeepingCentre (64, 12));
    area.removeFromRight (Space::m);

    // The readouts get what is left. In a narrow window the last ones give way first.
    for (const auto& [label, width] : { std::pair { &tempoLabel, 90 }, std::pair { &cpuLabel, 80 } })
    {
        label->setVisible (area.getWidth() >= width);
        label->setBounds (area.removeFromLeft (juce::jmin (width, area.getWidth())));
    }
}

void TopBar::timerCallback()
{
    auto* edit = getEdit();
    const bool playing = edit != nullptr && edit->getTransport().isPlaying();

    // The play button shows what clicking it does, and screen readers say so.
    if (const auto title = juce::String (playing ? "Stop" : "Play"); playButton.getTitle() != title)
    {
        playButton.setTitle (title);
        playButton.setIcon (playing ? Icon::stop : Icon::play);
    }

    playButton.setEnabled (edit != nullptr);

    juce::String tempo;

    if (edit != nullptr)
    {
        const auto position = edit->getTransport().getPosition();
        tempo = juce::String (edit->tempoSequence.getBpmAt (position), 1) + " BPM";
    }

    tempoLabel.setText (tempo, juce::dontSendNotification);
    cpuLabel.setText ("CPU " + juce::String (juce::roundToInt (engine.getDeviceManager().getCpuUsage() * 100.0)) + "%",
                      juce::dontSendNotification);
    updateKeyboardButton();

    // The master volume and meter follow the open project and its playback.
    const auto masterVolumePlugin = edit != nullptr ? edit->getMasterVolumePlugin() : nullptr;
    masterVolumeAttachment.setParameter (masterVolumePlugin.get(),
                                         masterVolumePlugin != nullptr ? masterVolumePlugin->volParam->paramID : juce::String());
    masterVolumeAttachment.update();

    auto* context = edit != nullptr ? edit->getCurrentPlaybackContext() : nullptr;
    masterMeter.setSource (context != nullptr ? &context->masterLevels : nullptr);
}
} // namespace phazite::ui
