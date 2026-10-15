// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "model/project/ProjectContext.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
TopBar::TopBar (te::Engine& e, CommandBridge& bridge, ComputerKeyboard& keyboard, const ProjectContext& context)
    : engine (e),
      commands (bridge),
      computerKeyboard (keyboard),
      projectContext (context)
{
    setTitle ("Transport");

    playButton.onClick = [this] { commands.invoke ("transport.playStop"); };
    loopButton.onClick = [this] { commands.invoke ("transport.loop"); };
    metronomeButton.onClicked = [this] (bool isMenuClick)
    {
        if (isMenuClick)
        {
            showMetronomeMenu();
            return;
        }

        commands.invoke ("transport.metronome");
        metronomeButton.setToggleState (isTicked ("transport.metronome"), juce::dontSendNotification);
    };
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
    for (auto* button : std::initializer_list<IconButton*> { &playButton, &loopButton, &metronomeButton, &keyboardButton, &searchButton })
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
    loopButton.setTooltip (withShortcut (commands,
                                         "Loop: at the end of the song, playback starts again from its start. The song ends with "
                                         "the bar its last clip ends in, so the loop grows as you add clips later",
                                         "transport.loop"));
    metronomeButton.setTooltip (withShortcut (
        commands, "Metronome: clicks on every beat while the song plays. Right-click for the count-in", "transport.metronome"));
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

void TopBar::showMetronomeMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "transport.metronome", "transport.countIn" });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (metronomeButton));
}

bool TopBar::isTicked (const juce::String& commandID) const
{
    const auto* command = commands.getRegistry().find (commandID);
    return command != nullptr && command->isTicked != nullptr && command->isTicked();
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
    loopButton.setBounds (area.removeFromLeft (44));
    metronomeButton.setBounds (area.removeFromLeft (44));
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
    auto* edit = projectContext.getEdit();
    const bool playing = edit != nullptr && edit->getTransport().isPlaying();

    // The play button shows what clicking it does, and screen readers say so.
    if (const auto title = juce::String (playing ? "Stop" : "Play"); playButton.getTitle() != title)
    {
        playButton.setTitle (title);
        playButton.setIcon (playing ? Icon::stop : Icon::play);
    }

    playButton.setEnabled (edit != nullptr);
    loopButton.setEnabled (edit != nullptr);
    loopButton.setToggleState (edit != nullptr && edit->getTransport().looping.get(), juce::dontSendNotification);
    metronomeButton.setEnabled (edit != nullptr);
    metronomeButton.setToggleState (isTicked ("transport.metronome"), juce::dontSendNotification);

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
