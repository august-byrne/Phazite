// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SettingsPanel.h"
#include "model/mixer/MixerModel.h"
#include "model/transport/TransportModel.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr int rowHeight = 28;
    constexpr int correctionLabelWidth = 100, correctionWidth = 160; // the recordings' correction's row

    void makeHeading (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    }

    // How tall a label's text is, wrapped to a width.
    int getTextHeight (const juce::Label& label, int width)
    {
        if (label.getText().isEmpty())
            return 0;

        juce::AttributedString text (label.getText());
        text.setFont (label.getFont());
        juce::TextLayout layout;
        layout.createLayout (text, (float) width);
        return (int) std::ceil (layout.getHeight()) + Space::xs;
    }
} // namespace

//==============================================================================
DelayDescription describeDelay (const EngineContext::AudioOutput& output, const DelayCompensation& compensation, bool lowLatency,
                                bool hasAsioDrivers)
{
    DelayDescription description;

    if (! output.isOpen)
    {
        description.lines.add ("No audio output is open, so nothing can be heard: choose one above.");
        return description;
    }

    const auto ms = [] (double seconds)
    {
        const auto milliseconds = seconds * 1000.0;
        return juce::String (milliseconds, milliseconds < 100.0 ? 1 : 0) + " ms";
    };

    const auto shortest = output.latencySeconds + (lowLatency ? 0.0 : compensation.seconds);
    auto& lines = description.lines;
    lines.add ("A note you play sounds " + ms (shortest) + " to " + ms (shortest + output.bufferSeconds) + " later"
               + (output.isBluetooth ? ", and Bluetooth adds more." : "."));
    lines.add ("The buffer: " + juce::String (output.bufferSamples) + " samples, " + ms (output.bufferSeconds)
               + ", which a note waits for at most.");
    lines.add ("The output: " + ms (output.latencySeconds) + ", as its driver reports it.");

    if (lowLatency)
    {
        lines.add ("Delay compensation: off, as Low Latency is on; Phazite's devices that look ahead are bypassed.");
    }
    else if (compensation.seconds > 0.0)
    {
        juce::StringArray late;

        for (const auto& [name, seconds] : compensation.devices)
            if (late.size() < 3)
                late.add (name + " (" + ms (seconds) + ")");

        lines.add ("Delay compensation: " + ms (compensation.seconds) + ", for " + late.joinIntoString (", ")
                   + ". Low Latency, in the top bar, turns it off while you play.");
    }
    else
    {
        lines.add ("Delay compensation: none, as no device in this song looks ahead.");
    }

    if (hasAsioDrivers && ! output.type.equalsIgnoreCase ("ASIO"))
        lines.add ("With an audio interface, choose ASIO above, then its driver: sound comes with less delay.");

    if (output.isBluetooth)
        description.warning = "This output is Bluetooth, which adds about 150 to 250 ms that no driver reports: fine for listening, "
                              "too slow for playing and recording. For those, plug in wired headphones or speakers and choose them "
                              "above.";

    return description;
}

//==============================================================================
AudioSettings::AudioSettings (EngineContext& e, const ProjectContext& context)
    : engine (e),
      projectContext (context),
      // No inputs to 512 inputs, 1 to 512 outputs, as stereo pairs; nothing behind a button. MIDI inputs are in their own
      // tab, because Tracktion (not JUCE's device manager) opens them.
      selector (e.getAudioDeviceManager(), 0, 512, 1, 512, false, false, true, false)
{
    setTitle ("Audio");
    viewport.setViewedComponent (&selector, false);
    viewport.setScrollBarsShown (true, false);
    makeHeading (delayHeading, "Delay");
    makeHeading (recordingHeading, "Recording");
    correctionLabel.setText ("Correction", juce::dontSendNotification);
    correctionNote.setText ("Moves what the audio inputs record earlier by this much, after the delays the driver reports. Leave it at "
                            "0 ms unless recordings come out late with this audio interface.",
                            juce::dontSendNotification);
    correction.setTitle ("Recording correction");
    correction.setSliderStyle (juce::Slider::IncDecButtons);
    correction.setRange (-100.0, 100.0, 0.1);
    correction.setTextValueSuffix (" ms");
    correction.setValue (engine.getRecordingCorrection(), juce::dontSendNotification);
    correction.onValueChange = [this] { engine.setRecordingCorrection (correction.getValue()); };

    for (auto* label : { &delayText, &warning, &correctionNote })
    {
        label->setJustificationType (juce::Justification::topLeft);
        label->setMinimumHorizontalScale (1.0f);
    }

    delayText.setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    correctionNote.setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    warning.setColour (juce::Label::textColourId, getPalette (*this).warning);

    for (auto* c : std::initializer_list<juce::Component*> { &viewport, &delayHeading, &delayText, &warning, &recordingHeading,
                                                             &correctionLabel, &correction, &correctionNote })
        addAndMakeVisible (c);

    engine.addChangeListener (this);
    startTimer (1000);
    update();
}

AudioSettings::~AudioSettings()
{
    engine.removeChangeListener (this);
}

void AudioSettings::update()
{
    // Kept for each input of each device: another device may have another.
    if (! correction.isMouseButtonDown (true))
        correction.setValue (engine.getRecordingCorrection(), juce::dontSendNotification);

    const auto description =
        describeDelay (engine.getAudioOutput(), getDelayCompensation (projectContext), engine.isLowLatency(), engine.hasAsioDrivers());
    const auto text = description.lines.joinIntoString ("\n");

    if (text != delayText.getText() || description.warning != warning.getText())
    {
        delayText.setText (text, juce::dontSendNotification);
        warning.setText (description.warning, juce::dontSendNotification);
        warning.setVisible (description.warning.isNotEmpty());
        resized();
    }
}

void AudioSettings::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void AudioSettings::resized()
{
    // The delay's text under the device settings, as tall as it needs, then the recordings' correction.
    auto area = getLocalBounds().reduced (Space::l);
    const auto textHeight = getTextHeight (delayText, area.getWidth());
    const auto warningHeight = warning.isVisible() ? getTextHeight (warning, area.getWidth()) + Space::s : 0;
    const auto noteHeight = getTextHeight (correctionNote, area.getWidth());
    auto recording = area.removeFromBottom (2 * rowHeight + noteHeight);
    area.removeFromBottom (Space::m);
    auto bottom = area.removeFromBottom (rowHeight + textHeight + warningHeight);
    area.removeFromBottom (Space::m);

    viewport.setBounds (area);
    selector.setSize (viewport.getMaximumVisibleWidth(), selector.getHeight()); // it sets its own height to what it shows

    delayHeading.setBounds (bottom.removeFromTop (rowHeight));
    delayText.setBounds (bottom.removeFromTop (textHeight));

    if (warning.isVisible())
        warning.setBounds (bottom.withTrimmedTop (Space::s));

    recordingHeading.setBounds (recording.removeFromTop (rowHeight));
    auto row = recording.removeFromTop (rowHeight);
    correctionLabel.setBounds (row.removeFromLeft (correctionLabelWidth));
    correction.setBounds (row.removeFromLeft (correctionWidth));
    correctionNote.setBounds (recording);
}

//==============================================================================
MidiSettings::MidiSettings (EngineContext& e, ComputerKeyboard& keyboard, const ProjectContext& context)
    : engine (e),
      computerKeyboard (keyboard),
      projectContext (context)
{
    makeHeading (inputsHeading, "MIDI inputs");
    makeHeading (keyboardHeading, "Computer keyboard");

    noInputsLabel.setText ("No MIDI devices found. Connect a MIDI keyboard: it shows up here within a few seconds.",
                           juce::dontSendNotification);
    keyboardHelp.setText ("A S D F G H J K L ; play the white keys from middle C, and W E T Y U O P the black keys. "
                          "Z and X change the octave, C and V the velocity. Hold Shift for single-key shortcuts.",
                          juce::dontSendNotification);

    for (auto* label : { &noInputsLabel, &keyboardHelp, &routingLabel })
    {
        label->setJustificationType (juce::Justification::topLeft);
        label->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    }

    keyboardButton.onClick = [this] { computerKeyboard.setEnabled (keyboardButton.getToggleState()); };

    for (auto* c : std::initializer_list<juce::Component*> { &inputsHeading, &noInputsLabel, &keyboardHeading, &keyboardButton,
                                                             &keyboardHelp, &routingLabel })
        addAndMakeVisible (c);

    engine.addChangeListener (this);
    rebuild();
}

MidiSettings::~MidiSettings()
{
    engine.removeChangeListener (this);
}

void MidiSettings::rebuild()
{
    inputButtons.clear();

    for (const auto& input : engine.getMidiInputs())
    {
        auto* button = inputButtons.add (std::make_unique<juce::ToggleButton> (input.name));
        button->setToggleState (input.isOn, juce::dontSendNotification);
        button->setTooltip ("Turns this MIDI input on or off. Everything it plays goes to the track that live MIDI plays.");
        button->onClick = [this, id = input.id, button] { engine.setMidiInputOn (id, button->getToggleState()); };
        addAndMakeVisible (button);
    }

    noInputsLabel.setVisible (inputButtons.isEmpty());
    keyboardButton.setToggleState (computerKeyboard.isEnabled(), juce::dontSendNotification);

    juce::String routing ("Live MIDI plays no track yet: it goes to the first track with an instrument.");

    if (const auto track = getChannel (projectContext, getLiveMidiTrack (projectContext)))
        routing = "Live MIDI plays the track \"" + track->name + "\".";

    routingLabel.setText (routing, juce::dontSendNotification);
    resized();
}

void MidiSettings::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void MidiSettings::resized()
{
    auto area = getLocalBounds().reduced (Space::l);

    inputsHeading.setBounds (area.removeFromTop (rowHeight));

    if (inputButtons.isEmpty())
        noInputsLabel.setBounds (area.removeFromTop (rowHeight * 2));

    for (auto* button : inputButtons)
        button->setBounds (area.removeFromTop (rowHeight));

    area.removeFromTop (Space::xl);
    keyboardHeading.setBounds (area.removeFromTop (rowHeight));
    keyboardButton.setBounds (area.removeFromTop (rowHeight));
    keyboardHelp.setBounds (area.removeFromTop (rowHeight * 2));

    area.removeFromTop (Space::xl);
    routingLabel.setBounds (area.removeFromTop (rowHeight * 2));
}

//==============================================================================
ShortcutSettings::ShortcutSettings (CommandBridge& commands, std::function<void (const juce::String&)> chooseKeymap)
    : editor (commands.getKeyMappings(), true)
{
    keymapLabel.setText ("Keymap", juce::dontSendNotification);
    keymapLabel.attachToComponent (&keymapBox, true);

    const auto keymaps = Keymap::getBuiltIn();

    for (int i = 0; i < keymaps.size(); ++i)
        keymapBox.addItem (keymaps[i].name, i + 1);

    keymapBox.setText (commands.getKeymap().name, juce::dontSendNotification);
    keymapBox.setTooltip ("Which app's shortcuts to start from. Your own changes are kept for each keymap.");
    keymapBox.onChange = [this, choose = std::move (chooseKeymap)] { choose (keymapBox.getText()); };

    addAndMakeVisible (keymapBox);
    addAndMakeVisible (editor);
}

void ShortcutSettings::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void ShortcutSettings::resized()
{
    auto area = getLocalBounds().reduced (Space::l);
    auto top = area.removeFromTop (controlHeight);
    keymapBox.setBounds (top.withTrimmedLeft (80).withWidth (220));
    area.removeFromTop (Space::m);
    editor.setBounds (area);
}

//==============================================================================
SettingsPanel::SettingsPanel (EngineContext& engine, CommandBridge& commands, ComputerKeyboard& computerKeyboard,
                              const ProjectContext& context, std::function<void (const juce::String&)> chooseKeymap)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Settings");
    setTabBarDepth (32);
    setOutline (0);

    const auto background = getPalette (*this).surface;

    // The Engine saves the audio settings' changes in its settings.
    addTab ("Audio", background, new AudioSettings (engine, context), true);
    addTab ("MIDI", background, new MidiSettings (engine, computerKeyboard, context), true);
    addTab ("Shortcuts", background, new ShortcutSettings (commands, std::move (chooseKeymap)), true);
}
} // namespace phazite::ui
