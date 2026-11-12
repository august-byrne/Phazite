// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TopBar.h"
#include "model/transport/RecordingModel.h"
#include "ui/components/ValueText.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <array>
#include <utility>

namespace phazite::ui
{
// Hears every press in the app while what the tempo stretches shows: one anywhere but on it or on the tempo field hides it.
struct TopBar::OutsideClick : public juce::MouseListener
{
    explicit OutsideClick (TopBar& b)
        : bar (b)
    {
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (auto* inside : std::initializer_list<juce::Component*> { &bar.tempoStretch, &bar.tempoField })
            if (e.originalComponent == inside || inside->isParentOf (e.originalComponent))
                return;

        bar.hideTempoStretch();
    }

    TopBar& bar;
};

TopBar::TopBar (CommandBridge& bridge, ComputerKeyboard& keyboard, ProjectContext& context)
    : commands (bridge),
      computerKeyboard (keyboard),
      projectContext (context),
      tempoField ("Tempo", minTempo, maxTempo, 1.0, 120.0),
      tempoStretch (bridge, context),
      outsideClick (std::make_unique<OutsideClick> (*this))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Transport");

    // Lessons point at controls by the commands they run (Lessons::Step::target).
    playButton.setComponentID ("transport.playStop");
    recordButton.setComponentID ("transport.record");
    metronomeButton.setComponentID ("transport.metronome");

    playButton.onClick = [this] { commands.invoke ("transport.playStop"); };
    recordButton.onClicked = [this] (bool isMenuClick)
    {
        // With nothing to record onto, a click offers what could record: a new track for an input.
        if (isMenuClick || ! isCommandEnabled ("transport.record"))
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
    masterVolume.setTooltip ("The volume of everything Phazite plays. Click it and type a level, or double-click to put it back where "
                             "it starts");

    // Clicking these buttons leaves the keyboard focus where it was; Tab still reaches them.
    for (auto* button : std::initializer_list<juce::Button*> { &playButton, &recordButton, &captureButton, &loopButton, &metronomeButton,
                                                               &lowLatencyButton, &timeSignatureButton, &keyboardButton })
    {
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &tempoField, &masterMeter, &masterVolume })
        addAndMakeVisible (c);

    // A/B, and nothing hidden: while a version's bounce is heard rather than the song, the bar says so.
    compareButton.setTitle ("A/B");
    compareButton.setMouseClickGrabsKeyboardFocus (false);
    compareButton.onClick = [this] { commands.invoke ("version.compare"); };
    addAndMakeVisible (compareButton);

    hearing.setTitle ("Hearing");
    hearing.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    hearing.setJustificationType (juce::Justification::centredLeft);
    hearing.setTooltip (
        "A commit's bounce is heard in place of the song. A/B, hearing the uncommitted changes, or any edit, plays the song again");
    addChildComponent (hearing);

    lookAndFeelChanged();
    projectContext.addListener (this);
    projectChanged();
    startTimerHz (4);
}

void TopBar::versionsChanged()
{
    updateCompare();
}

void TopBar::updateCompare()
{
    // What is heard, or bounced to be heard, while it isn't the song; and A/B lit meanwhile, saying what a press does.
    juce::String text = describeHearing (projectContext);
    const auto waiting = isWaitingToCompare (projectContext);

    if (waiting)
    {
        const auto compared = getCompareVersion (projectContext);
        const auto progress = getBounceProgress (projectContext, compared);
        text = "Bouncing to hear it"
               + (progress.has_value() ? ": " + juce::String (juce::roundToInt (*progress * 100.0f)) + "%" : juce::String());
    }
    else if (text.isEmpty())
    {
        text = getCompareProblem (projectContext); // why the last press couldn't hear it
    }

    if (hearing.getText() != text)
        hearing.setText (text, juce::dontSendNotification);

    hearing.setVisible (text.isNotEmpty());
    compareButton.setEnabled (canCompare (projectContext));
    compareButton.setToggleState (waiting || getHeardVersion (projectContext).isNotEmpty(), juce::dontSendNotification);

    if (const auto tip = withShortcut (commands, describeCompare (projectContext), "version.compare"); compareButton.getTooltip() != tip)
        compareButton.setTooltip (tip);
}

TopBar::~TopBar()
{
    hideTempoStretch();
    projectContext.removeListener (this);
}

void TopBar::projectChanged()
{
    tempo = getTempoHandle (projectContext); // ends a drag of the closing project's tempo
    masterVolumeAttachment.setParameter (getMasterVolume (projectContext));
    masterMeter.setSource (getMasterLevel (projectContext));
    timerCallback();
    versionsChanged();
}

void TopBar::refreshTooltips()
{
    playButton.setTooltip (withShortcut (commands, "Play or stop", "transport.playStop"));
    refreshRecordTooltip();
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
    updateCompare();
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
            hideTempoStretch();
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

    if (! std::exchange (hearsOutsideClicks, true))
        juce::Desktop::getInstance().addGlobalMouseListener (outsideClick.get());
}

void TopBar::hideTempoStretch()
{
    tempoStretch.setVisible (false);

    if (std::exchange (hearsOutsideClicks, false))
        juce::Desktop::getInstance().removeGlobalMouseListener (outsideClick.get());
}

void TopBar::showRecordMenu()
{
    juce::PopupMenu menu;

    if (! isCommandEnabled ("transport.record"))
        menu.addSectionHeader ("Nothing records yet: arm a track, or record an input on a new track");

    addCommandItems (menu, commands,
                     { "transport.record", "transport.countIn", "transport.punch", "transport.overdub", "transport.quantizeRecordings" });

    // A microphone or an instrument plugged in: a new track that records it, armed, the input chosen last first.
    auto& context = projectContext;
    auto inputs = getAudioInputs (context);
    const auto last = getLastInput (context);
    std::stable_partition (inputs.begin(), inputs.end(), [&last] (const AudioInput& input) { return input.id == last; });
    menu.addSeparator();

    if (inputs.empty())
    {
        menu.addItem ("Record an Input on a New Track (no audio inputs: choose an input device in the audio settings)", false, false,
                      nullptr);
    }
    else
    {
        juce::PopupMenu choices;

        for (const auto& input : inputs)
            choices.addItem (input.name, [&context, id = input.id] { addTrackForInput (context, id); });

        menu.addSubMenu ("Record an Input on a New Track", choices, context.hasProject());
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordButton));
}

void TopBar::refreshRecordTooltip()
{
    // What it records onto, as the tracks' arm switches show it, or what to do while it would record nothing.
    const auto text = recordTargets.isNotEmpty()
                          ? "Record onto " + recordTargets
                                + ", from where playback starts. Right-click to record an input on a new track, and for the "
                                  "count-in, punch-in, overdub and quantizing"
                          : juce::String ("Record: nothing records yet. Arm a track with the circle in its header: one with an instrument "
                                          "records what you play on MIDI, one without records an input. Click to record an input on "
                                          "a new track, and right-click for the count-in, punch-in, overdub and quantizing");
    recordButton.setTooltip (withShortcut (commands, text, "transport.record"));
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
    hearing.setColour (juce::Label::textColourId, getPalette (*this).focus); // which reads as text (4.5:1), as the accent may not
}

void TopBar::paint (juce::Graphics& g)
{
    // On the window's frame, under the title bar, as one with it: the panels' cards begin under it.
    g.fillAll (getPalette (*this).surface);
}

void TopBar::resized()
{
    // Along its top, under the title bar's controls, with the room under them that Shell::transportBarHeight keeps.
    auto area = getLocalBounds().reduced (Space::s, 0).removeFromTop (controlHeight);

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
    area.removeFromRight (Space::m);
    compareButton.setBounds (area.removeFromLeft (44));
    area.removeFromLeft (Space::s);
    hearing.setBounds (area);
}

void TopBar::timerCallback()
{
    const auto open = projectContext.hasProject();
    const bool playing = isPlaying (projectContext);

    // How far the bounce A/B waits for has come.
    if (isWaitingToCompare (projectContext))
        updateCompare();

    // The play button shows what clicking it does, and screen readers say so.
    if (const auto title = juce::String (playing ? "Stop" : "Play"); playButton.getTitle() != title)
    {
        playButton.setTitle (title);
        playButton.setIcon (playing ? Icon::stop : Icon::play);
    }

    playButton.setEnabled (open);
    recordButton.setEnabled (open); // with nothing to record onto, a click offers what could record (showRecordMenu())
    recordButton.setToggleState (isCommandTicked ("transport.record"), juce::dontSendNotification);

    if (const auto targets = open ? describeRecordTargets (projectContext) : juce::String(); targets != recordTargets)
    {
        recordTargets = targets;
        refreshRecordTooltip();
    }
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
