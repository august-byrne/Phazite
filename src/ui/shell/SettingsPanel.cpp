// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SettingsPanel.h"
#include "model/engine/LiveInput.h"
#include "ui/theme/LookAndFeel.h"

#include <juce_audio_utils/juce_audio_utils.h> // AudioDeviceSelectorComponent

namespace phazite::ui
{
namespace
{
    constexpr int rowHeight = 28;

    void makeHeading (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    }
} // namespace

//==============================================================================
MidiSettings::MidiSettings (te::Engine& e, ComputerKeyboard& keyboard, std::function<te::Edit*()> editGetter)
    : engine (e),
      computerKeyboard (keyboard),
      getEdit (std::move (editGetter))
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

    engine.getDeviceManager().addChangeListener (this);
    rebuild();
}

MidiSettings::~MidiSettings()
{
    engine.getDeviceManager().removeChangeListener (this);
}

void MidiSettings::rebuild()
{
    inputButtons.clear();

    for (const auto& device : engine.getDeviceManager().getMidiInDevices())
    {
        if (device->getDeviceType() != te::InputDevice::physicalMidiDevice)
            continue;

        auto* button = inputButtons.add (std::make_unique<juce::ToggleButton> (device->getName()));
        button->setToggleState (device->isEnabled(), juce::dontSendNotification);
        button->setTooltip ("Turns this MIDI input on or off. Everything it plays goes to the track that live MIDI plays.");

        // Tracktion may replace its device objects when devices change, so keep only a weak reference.
        button->onClick = [weakDevice = std::weak_ptr<te::MidiInputDevice> (device), button]
        {
            if (auto d = weakDevice.lock())
                d->setEnabled (button->getToggleState());
        };

        addAndMakeVisible (button);
    }

    noInputsLabel.setVisible (inputButtons.isEmpty());
    keyboardButton.setToggleState (computerKeyboard.isEnabled(), juce::dontSendNotification);

    juce::String routing ("Live MIDI plays no track yet: it goes to the first track with an instrument.");

    if (auto* edit = getEdit())
        if (auto* track = getLiveMidiTrack (*edit))
            routing = "Live MIDI plays the track \"" + track->getName() + "\".";

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
ShortcutSettings::ShortcutSettings (CommandRegistry& commands, std::function<void (const juce::String&)> chooseKeymap)
    : editor (*commands.getCommandManager().getKeyMappings(), true)
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
SettingsPanel::SettingsPanel (te::Engine& engine, CommandRegistry& commands, ComputerKeyboard& computerKeyboard,
                              std::function<te::Edit*()> getEdit, std::function<void (const juce::String&)> chooseKeymap)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Settings");
    setTabBarDepth (32);
    setOutline (0);

    const auto background = getPalette (*this).surface;

    // The device type (including ASIO on Windows), device, sample rate and buffer size; 0 to 512 inputs
    // and 1 to 512 outputs, shown as stereo pairs, with the advanced options behind a button. MIDI
    // inputs are in their own tab, because Tracktion (not JUCE's device manager) opens them. The
    // Engine saves changes in its settings.
    addTab ("Audio", background,
            new juce::AudioDeviceSelectorComponent (engine.getDeviceManager().deviceManager, 0, 512, 1, 512, false, false, true, true),
            true);
    addTab ("MIDI", background, new MidiSettings (engine, computerKeyboard, std::move (getEdit)), true);
    addTab ("Shortcuts", background, new ShortcutSettings (commands, std::move (chooseKeymap)), true);
}
} // namespace phazite::ui
