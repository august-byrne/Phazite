// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "LookAndFeel.h"

namespace phazite::ui
{
TopBar::TopBar (te::Engine& e, CommandRegistry& registry, std::function<te::Edit*()> editGetter)
    : engine (e),
      commands (registry),
      getEdit (std::move (editGetter))
{
    setTitle ("Transport");

    playButton.onClick = [this] { commands.invoke ("transport.playStop"); };

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
    addAndMakeVisible (searchButton);

    timerCallback();
    startTimerHz (4);
}

void TopBar::refreshTooltips()
{
    setTooltipWithShortcut (playButton, "Play or stop", "transport.playStop");
    setTooltipWithShortcut (searchButton, "Find and run any command by typing part of its name", "view.commandPalette");
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
}
} // namespace phazite::ui
