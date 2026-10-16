// SPDX-License-Identifier: AGPL-3.0-or-later
// The engine as the UI reaches it (architecture phase A3), without Tracktion's types: the audio devices, how busy the
// engine is, and the MIDI inputs. The app's services make the one EngineContext for the Engine (src/app/AppServices.h,
// with internal/EngineContext.h). Message thread only.
#pragma once

#include "model/mixer/DeviceType.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <vector>

namespace phazite
{
class EngineContext
{
public:
    ~EngineContext();

    // JUCE's device manager, which the audio settings show; the Engine saves what they change in its settings.
    juce::AudioDeviceManager& getAudioDeviceManager();

    // How much of the time available for each audio block the engine uses, from 0 to 1.
    double getCpuUsage() const;

    // The MIDI devices connected, each on or off. The Engine opens them (not JUCE's device manager), and what they
    // play goes to the track live MIDI plays.
    struct MidiInput
    {
        juce::String id, name;
        bool isOn = false;
    };

    std::vector<MidiInput> getMidiInputs() const;
    void setMidiInputOn (const juce::String& id, bool); // an ID that is gone changes nothing

    // The extensions of the audio files the engine reads, as ".wav"; and the devices that can be added (Mixer.h's
    // getDeviceTypes()), as the browser lists them.
    juce::StringArray getAudioFileExtensions() const;
    std::vector<DeviceType> getDeviceTypes() const;

    // Hears when audio or MIDI devices come, go or change.
    void addChangeListener (juce::ChangeListener*);
    void removeChangeListener (juce::ChangeListener*);

    struct Impl; // the Engine (internal/EngineContext.h)
    Impl& getImpl() const noexcept { return *impl; }
    explicit EngineContext (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (EngineContext)
};
} // namespace phazite
