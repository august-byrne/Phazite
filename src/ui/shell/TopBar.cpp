// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "ui/components/ValueText.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <array>

namespace phazite::ui
{
TopBar::TopBar (CommandBridge& bridge, ComputerKeyboard& keyboard, ProjectContext& context)
    : commands (bridge),
      computerKeyboard (keyboard),
      projectContext (context),
      tempoField ("Tempo", minTempo, maxTempo, 1.0, 120.0),
      tempoStretch (bridge, context)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
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
    captureButton.onClick = [this] { commands.invoke ("transport.capture"); };
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
    lowLatencyButton.onClick = [this] { commands.invoke ("transport.lowLatency"); };
    keyboardButton.onClick = [this] { commands.invoke ("input.computerKeyboard"); };

    // The tempo: each drag is one undo step, as are a typed tempo and each step of the wheel or the arrow keys.
    tempoField.textFromValueFunction = [] (double bpm) { return formatTempo (bpm); };
    tempoField.valueFromTextFunction = [this] (const juce::String& text) { return parseTempo (text).value_or (tempoField.getValue()); };
    tempoField.updateText();
    tempoField.onDragStart = [this]
    {
        tempo.beginGesture();
        showTempoStretch();
    };
    tempoField.onDragEnd = [this] { tempo.endGesture(); };
    tempoField.onValueChange = [this]
    {
        tempo.setValue (tempoField.getValue());
        showTempoStretch();
    };
    tempoField.setTooltip ("Tempo, in beats per minute: drag it up or down, scroll, or click it and type. As it changes, it shows "
                           "what it stretches");

    timeSignatureButton.setTitle ("Time signature");
    timeSignatureButton.setTooltip ("Time signature: the beats in a bar, and the note that counts as a beat. Click for others");
    timeSignatureButton.onClick = [this] { showTimeSignatureMenu(); };
    refreshTooltips();

    showAsDecibels (masterVolume);
    masterVolume.setTooltip ("The volume of everything Phazite plays. Double-click for 0 dB");

    // Clicking these buttons leaves the keyboard focus where it was; Tab still reaches them.
    for (auto* button : std::initializer_list<juce::Button*> { &playButton, &recordButton, &captureButton, &loopButton, &metronomeButton,
                                                               &lowLatencyButton, &timeSignatureButton, &keyboardButton })
    {
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &tempoField, &masterMeter, &masterVolume })
        addAndMakeVisible (c);

    lookAndFeelChanged();
    projectContext.addListener (this);
    projectChanged();
    startTimerHz (4);
}

TopBar::~TopBar()
{
    projectContext.removeListener (this);
}

void TopBar::projectChanged()
{
    tempo = getTempoHandle (projectContext); // ends a drag of the closing project's tempo
    masterVolumeAttachment.setParameter (getMasterVolume (projectContext));
    masterMeter.setSource (getMasterLevel (projectContext));
    timerCallback();
}

void TopBar::refreshTooltips()
{
    playButton.setTooltip (withShortcut (commands, "Play or stop", "transport.playStop"));
    recordButton.setTooltip (withShortcut (commands,
                                           "Record what you play onto the selected track, from where playback starts. Right-click "
                                           "for the count-in, overdub and quantizing",
                                           "transport.record"));
    captureButton.setTooltip (withShortcut (commands,
                                            "Capture: turns what you played in the last 30 seconds into a clip on the selected "
                                            "track, recording or not",
                                            "transport.capture"));
    loopButton.setTooltip (withShortcut (commands,
                                         "Loop: at the end of the song, playback starts again from its start. The song ends with "
                                         "the bar its last clip ends in, so the loop grows as you add clips later",
                                         "transport.loop"));
    metronomeButton.setTooltip (withShortcut (
        commands, "Metronome: clicks on every beat while the song plays. Right-click for the count-in", "transport.metronome"));
    lowLatencyButton.setTooltip (withShortcut (commands,
                                               "Low latency: while on, what you play has no delay to wait for. Devices that look ahead "
                                               "are bypassed, delay compensation is off and the buffer shrinks. Turn it off to mix",
                                               "transport.lowLatency"));

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
    const auto timeSignature = getTimeSignature (projectContext);
    tempoField.setEnabled (tempo.isValid());
    timeSignatureButton.setEnabled (timeSignature.has_value());

    if (! tempo.isValid() || ! timeSignature.has_value())
        return;

    // Not while it is dragged or typed in. (Its text box keeps the keyboard focus after a click, typing or not.)
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const auto typing = dynamic_cast<const juce::TextEditor*> (focused) != nullptr && tempoField.isParentOf (focused);

    if (! tempo.isInGesture() && ! typing)
        tempoField.setValue (tempo.getValue(), juce::dontSendNotification);

    timeSignatureButton.setButtonText (formatTimeSignature (timeSignature->numerator, timeSignature->denominator));

    // What the tempo stretches goes once the tempo has stayed as it is a while, and isn't being typed or pointed at.
    if (tempoStretch.isVisible())
    {
        if (tempo.isInGesture() || typing || tempoStretch.isMouseOverOrDragging (true) || tempoField.isMouseOverOrDragging (true))
            tempoStretchShownAt = juce::Time::getMillisecondCounter();
        else if (juce::Time::getMillisecondCounter() - tempoStretchShownAt > (juce::uint32) Shell::tempoStretchSeconds * 1000)
            tempoStretch.setVisible (false);
        else
            tempoStretch.update();
    }
}

void TopBar::showTempoStretch()
{
    // Over whatever is under the top bar, as wide as it needs, in the window.
    auto* window = getTopLevelComponent();
    const auto stretch = getTempoStretch (projectContext);

    if (window == nullptr || window == this || stretch.samples + stretch.patterns == 0)
        return;

    if (tempoStretch.getParentComponent() != window)
        window->addChildComponent (tempoStretch);

    const auto field = window->getLocalArea (&tempoField, tempoField.getLocalBounds());
    const auto width = std::min (Shell::tempoStretchWidth, window->getWidth());
    tempoStretch.setBounds (juce::jlimit (0, window->getWidth() - width, field.getX()), field.getBottom() + Space::xs, width,
                            tempoStretch.getIdealHeight());
    tempoStretch.update();
    tempoStretch.setVisible (true);
    tempoStretch.toFront (false);
    tempoStretchShownAt = juce::Time::getMillisecondCounter();
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
    const auto shown = getTimeSignature (projectContext);

    if (! shown.has_value())
        return;

    const auto current = *shown;
    const juce::Component::SafePointer<TopBar> safeThis (this);
    juce::PopupMenu menu;

    const auto addItem = [&menu, &current, safeThis] (TimeSignature timeSignature)
    {
        menu.addItem (formatTimeSignature (timeSignature.numerator, timeSignature.denominator), true, timeSignature == current,
                      [safeThis, timeSignature]
                      {
                          if (safeThis != nullptr)
                              setTimeSignature (safeThis->projectContext, timeSignature);
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
    captureButton.setBounds (area.removeFromLeft (44));
    loopButton.setBounds (area.removeFromLeft (44));
    metronomeButton.setBounds (area.removeFromLeft (44));
    lowLatencyButton.setBounds (area.removeFromLeft (44));
    area.removeFromLeft (Space::m);
    tempoField.setBounds (area.removeFromLeft (96));
    area.removeFromLeft (Space::xs);
    timeSignatureButton.setBounds (area.removeFromLeft (52));
    area.removeFromLeft (Space::m);

    keyboardButton.setBounds (area.removeFromRight (76));
    area.removeFromRight (Space::m);
    masterVolume.setBounds (area.removeFromRight (120));
    area.removeFromRight (Space::xs);
    masterMeter.setBounds (area.removeFromRight (64).withSizeKeepingCentre (64, 12));
}

void TopBar::timerCallback()
{
    const auto open = projectContext.hasProject();
    const bool playing = isPlaying (projectContext);

    // The play button shows what clicking it does, and screen readers say so.
    if (const auto title = juce::String (playing ? "Stop" : "Play"); playButton.getTitle() != title)
    {
        playButton.setTitle (title);
        playButton.setIcon (playing ? Icon::stop : Icon::play);
    }

    playButton.setEnabled (open);
    recordButton.setEnabled (isCommandEnabled ("transport.record"));
    recordButton.setToggleState (isCommandTicked ("transport.record"), juce::dontSendNotification);
    captureButton.setEnabled (isCommandEnabled ("transport.capture"));
    loopButton.setEnabled (open);
    loopButton.setToggleState (isLoopOn (projectContext), juce::dontSendNotification);
    metronomeButton.setEnabled (open);
    metronomeButton.setToggleState (isCommandTicked ("transport.metronome"), juce::dontSendNotification);
    lowLatencyButton.setEnabled (open);
    lowLatencyButton.setToggleState (isCommandTicked ("transport.lowLatency"), juce::dontSendNotification);
    updateTempo();

    updateKeyboardButton();

    masterVolumeAttachment.update();
}
} // namespace phazite::ui
