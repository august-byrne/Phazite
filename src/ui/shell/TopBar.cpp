// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "model/commands/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/transport/Tempo.h"
#include "ui/components/ValueText.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <array>

namespace phazite::ui
{
namespace
{
    // The time signatures the menu offers, as most DAWs do.
    constexpr std::array<TimeSignature, 8> commonTimeSignatures {
        { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } }
    };
} // namespace

TopBar::TopBar (te::Engine& e, CommandBridge& bridge, ComputerKeyboard& keyboard, const ProjectContext& context)
    : engine (e),
      commands (bridge),
      computerKeyboard (keyboard),
      projectContext (context),
      tempoField ("Tempo", minTempo, maxTempo, 1.0, 120.0)
{
    setTitle ("Transport");

    playButton.onClick = [this] { commands.invoke ("transport.playStop"); };
    recordButton.onClicked = [this] (bool isMenuClick)
    {
        if (isMenuClick)
        {
            showRecordMenu();
            return;
        }

        commands.invoke ("transport.record");
        recordButton.setToggleState (isCommandTicked ("transport.record"), juce::dontSendNotification);
    };
    loopButton.onClick = [this] { commands.invoke ("transport.loop"); };
    metronomeButton.onClicked = [this] (bool isMenuClick)
    {
        if (isMenuClick)
        {
            showMetronomeMenu();
            return;
        }

        commands.invoke ("transport.metronome");
        metronomeButton.setToggleState (isCommandTicked ("transport.metronome"), juce::dontSendNotification);
    };
    keyboardButton.onClick = [this] { commands.invoke ("input.computerKeyboard"); };

    searchButton.setButtonText ("Search commands");
    searchButton.onClick = [this] { commands.invoke ("view.commandPalette"); };

    // The tempo: each drag is one undo step, as are a typed tempo and each step of the wheel or the arrow keys.
    tempoField.textFromValueFunction = [] (double bpm) { return formatTempo (bpm); };
    tempoField.valueFromTextFunction = [this] (const juce::String& text) { return parseTempo (text).value_or (tempoField.getValue()); };
    tempoField.updateText();
    tempoField.onDragStart = [this]
    {
        if (auto* edit = projectContext.getEdit())
            tempoDrag = std::make_unique<phazite::Gesture> (*edit, "Change Tempo");
    };
    tempoField.onDragEnd = [this] { tempoDrag = nullptr; };
    tempoField.onValueChange = [this]
    {
        if (auto* edit = projectContext.getEdit())
            setTempo (*edit, tempoField.getValue());
    };
    tempoField.setTooltip ("Tempo, in beats per minute: drag it up or down, scroll, or click it and type");

    timeSignatureButton.setTitle ("Time signature");
    timeSignatureButton.setTooltip ("Time signature: the beats in a bar, and the note that counts as a beat. Click for others");
    timeSignatureButton.onClick = [this] { showTimeSignatureMenu(); };
    refreshTooltips();

    cpuLabel.setFont (LookAndFeel::getNumberFont (TextSize::body));
    cpuLabel.setJustificationType (juce::Justification::centred);
    cpuLabel.setTooltip ("How much of the time available for each audio block the engine uses");

    showAsDecibels (masterVolume);
    masterVolume.setTooltip ("The volume of everything Phazite plays. Double-click for 0 dB");

    // Clicking these buttons leaves the keyboard focus where it was; Tab still reaches them.
    for (auto* button : std::initializer_list<juce::Button*> { &playButton, &recordButton, &loopButton, &metronomeButton,
                                                               &timeSignatureButton, &keyboardButton, &searchButton })
    {
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &tempoField, &cpuLabel, &masterMeter, &masterVolume })
        addAndMakeVisible (c);

    lookAndFeelChanged();
    timerCallback();
    startTimerHz (4);
}

TopBar::~TopBar() = default;

void TopBar::refreshTooltips()
{
    playButton.setTooltip (withShortcut (commands, "Play or stop", "transport.playStop"));
    recordButton.setTooltip (withShortcut (commands,
                                           "Record what you play onto the selected track, from where playback starts. Right-click "
                                           "for the count-in, overdub and quantizing",
                                           "transport.record"));
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

void TopBar::updateTempo()
{
    auto* edit = projectContext.getEdit();
    tempoField.setEnabled (edit != nullptr);
    timeSignatureButton.setEnabled (edit != nullptr);

    if (edit == nullptr)
        return;

    // Not while it is dragged or typed in. (Its text box keeps the keyboard focus after a click, typing or not.)
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const auto typing = dynamic_cast<const juce::TextEditor*> (focused) != nullptr && tempoField.isParentOf (focused);

    if (tempoDrag == nullptr && ! typing)
        tempoField.setValue (getTempo (*edit), juce::dontSendNotification);

    const auto timeSignature = getTimeSignature (*edit);
    timeSignatureButton.setButtonText (formatTimeSignature (timeSignature.numerator, timeSignature.denominator));
}

void TopBar::showRecordMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "transport.record", "transport.countIn", "transport.overdub", "transport.quantizeRecordings" });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordButton));
}

void TopBar::showMetronomeMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "transport.metronome", "transport.countIn" });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (metronomeButton));
}

void TopBar::showTimeSignatureMenu()
{
    auto* edit = projectContext.getEdit();

    if (edit == nullptr)
        return;

    const auto current = getTimeSignature (*edit);
    const juce::Component::SafePointer<TopBar> safeThis (this);
    juce::PopupMenu menu;

    const auto addItem = [&menu, &current, safeThis] (TimeSignature timeSignature)
    {
        menu.addItem (formatTimeSignature (timeSignature.numerator, timeSignature.denominator), true, timeSignature == current,
                      [safeThis, timeSignature]
                      {
                          if (safeThis != nullptr)
                              if (auto* e = safeThis->projectContext.getEdit())
                                  setTimeSignature (*e, timeSignature);
                      });
    };

    // One the menu doesn't offer, from a project made elsewhere, comes first.
    if (std::find (commonTimeSignatures.begin(), commonTimeSignatures.end(), current) == commonTimeSignatures.end())
    {
        addItem (current);
        menu.addSeparator();
    }

    for (const auto timeSignature : commonTimeSignatures)
        addItem (timeSignature);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (timeSignatureButton));
}

bool TopBar::isCommandEnabled (const juce::String& commandID) const
{
    const auto* command = commands.getRegistry().find (commandID);
    return command != nullptr && CommandRegistry::isEnabled (*command);
}

bool TopBar::isCommandTicked (const juce::String& commandID) const
{
    const auto* command = commands.getRegistry().find (commandID);
    return command != nullptr && command->isTicked != nullptr && command->isTicked();
}

void TopBar::lookAndFeelChanged()
{
    recordButton.setColour (juce::TextButton::buttonOnColourId, getPalette (*this).danger);
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
    recordButton.setBounds (area.removeFromLeft (44));
    loopButton.setBounds (area.removeFromLeft (44));
    metronomeButton.setBounds (area.removeFromLeft (44));
    area.removeFromLeft (Space::m);
    tempoField.setBounds (area.removeFromLeft (96));
    area.removeFromLeft (Space::xs);
    timeSignatureButton.setBounds (area.removeFromLeft (52));
    area.removeFromLeft (Space::m);

    searchButton.setBounds (area.removeFromRight (180));
    area.removeFromRight (Space::s);
    keyboardButton.setBounds (area.removeFromRight (76));
    area.removeFromRight (Space::m);
    masterVolume.setBounds (area.removeFromRight (120));
    area.removeFromRight (Space::xs);
    masterMeter.setBounds (area.removeFromRight (64).withSizeKeepingCentre (64, 12));
    area.removeFromRight (Space::m);

    // The CPU readout gets what is left, and gives way in a narrow window.
    constexpr int cpuWidth = 80;
    cpuLabel.setVisible (area.getWidth() >= cpuWidth);
    cpuLabel.setBounds (area.removeFromLeft (juce::jmin (cpuWidth, area.getWidth())));
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
    recordButton.setEnabled (isCommandEnabled ("transport.record"));
    recordButton.setToggleState (isCommandTicked ("transport.record"), juce::dontSendNotification);
    loopButton.setEnabled (edit != nullptr);
    loopButton.setToggleState (edit != nullptr && edit->getTransport().looping.get(), juce::dontSendNotification);
    metronomeButton.setEnabled (edit != nullptr);
    metronomeButton.setToggleState (isCommandTicked ("transport.metronome"), juce::dontSendNotification);
    updateTempo();

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
