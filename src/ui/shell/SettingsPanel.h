// SPDX-License-Identifier: AGPL-3.0-or-later
// The settings window's content: the Audio, MIDI and Shortcuts tabs.
#pragma once

#include "model/engine/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace phazite
{
class ProjectContext;
} // namespace phazite

namespace phazite::ui
{
//==============================================================================
// What the audio settings say about delay, a sentence a line (docs/design.md, section 20): how long a note played takes to
// sound, from the output's buffer and latency and the song's delay compensation; whether low latency is on; and that an
// audio interface's ASIO driver gives less. And, apart, the warning that the output is Bluetooth, if it is.
struct DelayDescription
{
    juce::StringArray lines;
    juce::String warning;
};

DelayDescription describeDelay (const EngineContext::AudioOutput&, const DelayCompensation&, bool lowLatency, bool hasAsioDrivers);

//==============================================================================
// The Audio tab: JUCE's device settings, all of them showing (the driver, the device, the sample rate and the buffer,
// whose list gives each in milliseconds), then what they mean for delay (describeDelay()), kept up to date.
class AudioSettings : public juce::Component,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    AudioSettings (EngineContext&, const ProjectContext&);
    ~AudioSettings() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // For tests.
    juce::String getDelayText() const { return delayText.getText(); }
    juce::String getWarning() const { return warning.isVisible() ? warning.getText() : juce::String(); }

private:
    EngineContext& engine;
    const ProjectContext& projectContext;
    juce::AudioDeviceSelectorComponent selector;
    juce::Viewport viewport; // the device settings, which grow with what they show
    juce::Label delayHeading, delayText, warning;

    // Recording: the correction for a driver that reports its delays wrongly (EngineContext::getRecordingCorrection()).
    juce::Label recordingHeading, correctionLabel, correctionNote;
    juce::Slider correction;

    void update();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { update(); } // the devices changed
    void timerCallback() override { update(); }                                   // so might the song's devices
};

//==============================================================================
// The MIDI tab: the MIDI input devices, each with an on/off switch, the computer keyboard, and the
// track that live MIDI plays.
class MidiSettings : public juce::Component,
                     private juce::ChangeListener
{
public:
    MidiSettings (EngineContext&, ComputerKeyboard&, const ProjectContext&);
    ~MidiSettings() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    EngineContext& engine;
    ComputerKeyboard& computerKeyboard;
    const ProjectContext& projectContext;

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
    ShortcutSettings (CommandBridge&, std::function<void (const juce::String& keymapName)> chooseKeymap);

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

    SettingsPanel (EngineContext&, CommandBridge&, ComputerKeyboard&, const ProjectContext&,
                   std::function<void (const juce::String& keymapName)> chooseKeymap);

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
};
} // namespace phazite::ui
