#pragma once

#include <juce_audio_utils/juce_audio_utils.h> // AudioDeviceSelectorComponent, PluginListComponent
#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion; // same alias as Tracktion's examples

//==============================================================================
// One window: Play/Stop (4OSC on track 1 looping a 4-bar arpeggio), Audio Settings, Scan Plugins.
class MainComponent final : public juce::Component,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    explicit MainComponent (te::Engine&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;

    juce::TextButton playStopButton { "Play" };
    juce::TextButton audioSettingsButton { "Audio Settings" };
    juce::TextButton scanPluginsButton { "Scan Plugins" };
    juce::Label statusLabel;

    // Dialogs opened with launchAsync() delete themselves when closed. These weak pointers let us close
    // them before the Engine they refer to (device manager, plug-in list) is destroyed.
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow, pluginListWindow;

    void createEdit();
    void togglePlay();
    void showAudioSettings();
    void showPluginList();
    void updatePlayButtonText();

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // transport started/stopped
    void timerCallback() override;                                   // refreshes the status line

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
