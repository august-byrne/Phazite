// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainComponent.h"
#include "model/EngineSetup.h"

using namespace tracktion::literals; // 0_tp (time position), 16_bp (beat position), ...

namespace
{
// The demos' "lead" patch: a saw wave (waveShape1 = 3) through a low-pass filter (filterType = 1).
// Optional: 4OSC's default patch (one sine, no filter) is audible too.
constexpr const char* leadPatchXml = R"(<PLUGIN type="4osc" filterType="1" waveShape1="3" filterFreq="100"/>)";

// C - Am - F - G, one chord per bar.
constexpr int chordNotes[4][4] = { { 60, 64, 67, 72 },   // C
                                   { 57, 60, 64, 69 },   // Am
                                   { 53, 57, 60, 65 },   // F
                                   { 55, 59, 62, 67 } }; // G

// Eighth notes going up and down the chord: 8 steps per 4/4 bar.
constexpr int arpOrder[8] = { 0, 1, 2, 3, 2, 1, 0, 1 };
} // namespace

//==============================================================================
MainComponent::MainComponent (te::Engine& e)
    : engine (e)
{
    createEdit();

    // Tracktion sends MIDI controller mappings and app commands to the focused Edit.
    phazite::getUIBehaviour (engine).setFocusedEdit (edit.get());

    playStopButton.onClick = [this] { togglePlay(); };
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    scanPluginsButton.onClick = [this] { showPluginList(); };

    if (edit != nullptr)
        edit->getTransport().addChangeListener (this);
    else
        playStopButton.setEnabled (false);

    statusLabel.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (playStopButton);
    addAndMakeVisible (audioSettingsButton);
    addAndMakeVisible (scanPluginsButton);
    addAndMakeVisible (statusLabel);

    timerCallback();
    startTimerHz (4); // runs on the message thread, never the audio thread
    setSize (560, 140);
}

MainComponent::~MainComponent()
{
    stopTimer();

    // Close dialogs that point into the Engine, which is destroyed right after this component.
    delete audioSettingsWindow.getComponent();
    delete pluginListWindow.getComponent();

    if (edit != nullptr)
        edit->getTransport().removeChangeListener (this);

    phazite::getUIBehaviour (engine).setFocusedEdit (nullptr);
    edit = nullptr; // on the message thread, before the Engine
}

//==============================================================================
void MainComponent::createEdit()
{
    // An in-memory Edit (no file): 120 BPM, 4/4, one audio track routed to the default output.
    // Returns nullptr if construction failed.
    edit = te::Edit::createSingleTrackEdit (engine);

    if (edit == nullptr)
        return;

    auto* track = te::getAudioTracks (*edit)[0]; // track 1

    if (track == nullptr)
        return;

    // Insert 4OSC at index 0, i.e. before the track's default volume and level-meter plug-ins.
    // Keep the pointer until it is inserted: the plug-in cache drops unreferenced plug-ins.
    auto plugin = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});

    if (auto* synth = dynamic_cast<te::FourOscPlugin*> (plugin.get()))
    {
        if (auto patch = juce::ValueTree::fromXml (leadPatchXml); patch.isValid())
            synth->restorePluginStateFromValueTree (patch);

        track->pluginList.insertPlugin (plugin, 0, nullptr);
    }

    // A 4-bar MIDI clip starting at bar 1: 16 beats, which is 8 seconds at 120 BPM.
    const auto clipTime = edit->tempoSequence.toTime (te::BeatRange (0_bp, 16_bp));
    auto clip = track->insertMIDIClip ("Arpeggio", clipTime, nullptr);

    if (clip == nullptr)
        return;

    auto& sequence = clip->getSequence(); // note positions are in beats from the clip start

    for (int bar = 0; bar < 4; ++bar)
        for (int step = 0; step < 8; ++step)
            sequence.addNote (chordNotes[bar][arpOrder[step]],                      // MIDI note number
                              te::BeatPosition::fromBeats (bar * 4.0 + step * 0.5), // start
                              te::BeatDuration::fromBeats (0.45),                   // a bit under an eighth note
                              100,                                                  // velocity (1-127)
                              0,                                                    // colour index (for UIs)
                              nullptr);                                             // no undo for this setup

    // Loop the transport around the clip (like EngineHelpers::loopAroundClip, without starting playback).
    auto& transport = edit->getTransport();
    transport.setLoopRange (clip->getEditTimeRange());
    transport.looping = true;
    transport.setPosition (0_tp);
}

void MainComponent::togglePlay()
{
    if (edit == nullptr)
        return;

    auto& transport = edit->getTransport();

    if (transport.isPlaying())
    {
        transport.stop (false, false); // (discardRecordings, clearDevices)
        transport.setPosition (0_tp);  // Stop rewinds to bar 1
    }
    else
    {
        transport.play (false); // builds the playback graph on first use
    }
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    if (audioSettingsWindow != nullptr)
    {
        audioSettingsWindow->toFront (true);
        return;
    }

    // Same arguments as EngineHelpers::showAudioDeviceSettings in Tracktion's examples/common/Utilities.h:
    // 0-512 input and 1-512 output channels, no MIDI input or output options, channels shown as stereo
    // pairs, advanced options behind a button.
    auto& deviceManager = engine.getDeviceManager().deviceManager;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = TRANS ("Audio Settings");
    o.dialogBackgroundColour = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    o.content.setOwned (new juce::AudioDeviceSelectorComponent (deviceManager, 0, 512, 1, 512, false, false, true, true));
    o.content->setSize (400, 600);
    o.componentToCentreAround = this;

    audioSettingsWindow = o.launchAsync(); // the Engine saves device changes to Settings.xml
}

void MainComponent::showPluginList()
{
    if (pluginListWindow != nullptr)
    {
        pluginListWindow->toFront (true);
        return;
    }

    // Same construction as DemoRunner's plug-in list in Tracktion's examples.
    auto& pm = engine.getPluginManager();

    auto* list = new juce::PluginListComponent (pm.pluginFormatManager, pm.knownPluginList,
                                                engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
                                                &engine.getPropertyStorage().getPropertiesFile(),
                                                true); // allow plug-ins that load asynchronously (AUv3)

    // Scan on one background thread so the UI stays responsive; Tracktion's scanner isn't safe with more.
    list->setNumberOfThreadsForScanning (1);
    list->setSize (800, 600);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = TRANS ("Plug-ins");
    o.dialogBackgroundColour = juce::Colours::black;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.useBottomRightCornerResizer = true;
    o.componentToCentreAround = this;
    o.content.setOwned (list);

    pluginListWindow = o.launchAsync();

    // Open a VST3 scan (same as "Options... > Scan for new or updated VST3 plug-ins"). JUCE first asks which
    // folders to scan (the standard VST3 folders are filled in); clicking Scan starts it. The Engine saves
    // the resulting list to Settings.xml whenever it changes.
    for (auto* format : pm.pluginFormatManager.getFormats())
        if (format->getName() == "VST3")
            list->scanFor (*format);
}

//==============================================================================
void MainComponent::updatePlayButtonText()
{
    const bool playing = edit != nullptr && edit->getTransport().isPlaying();
    playStopButton.setButtonText (playing ? "Stop" : "Play");
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updatePlayButtonText();
}

void MainComponent::timerCallback()
{
    auto& dm = engine.getDeviceManager();
    auto* device = dm.deviceManager.getCurrentAudioDevice();

    juce::String text = device != nullptr ? device->getName() : juce::String ("No audio device");
    text << "  |  " << juce::String (dm.getSampleRate(), 0) << " Hz, " << dm.getBlockSize() << " samples  |  CPU "
         << juce::roundToInt (dm.getCpuUsage() * 100.0f) << "%";

    statusLabel.setText (text, juce::dontSendNotification);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto buttons = r.removeFromTop (36);
    const int w = buttons.getWidth() / 3;

    playStopButton.setBounds (buttons.removeFromLeft (w).reduced (4));
    audioSettingsButton.setBounds (buttons.removeFromLeft (w).reduced (4));
    scanPluginsButton.setBounds (buttons.reduced (4));
    statusLabel.setBounds (r.reduced (4));
}
