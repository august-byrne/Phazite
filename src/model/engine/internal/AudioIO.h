// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio device and MIDI inputs Phazite plays through, opened by Phazite rather than Tracktion
// (docs/tracktion-migration.md, T4): JUCE's device manager and MIDI inputs, one set for each Engine, which every engine
// context of it shares (getFor(), as the plug-in host is shared), opened as the app's settings saved them (the app's
// AppServices: Settings) and kept there as they change. Tracktion's own device manager opens nothing
// (EngineBehaviour::autoInitialiseDeviceManager()). What the MIDI inputs and the computer keyboard play goes to the engine
// that plays the open project, into the track live MIDI plays (setLiveMidi(), from model/playback/). With the MIDI
// recordings' options and the recordings' correction, which are Phazite's settings too. Message thread, but where it says.
#pragma once

#include "engine/core/Engine.h"
#include "model/base/internal/Tracktion.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <vector>

namespace phazite
{
class AudioIO final : public juce::ChangeBroadcaster, // the devices came, went or changed, or an option changed
                      private juce::ChangeListener,
                      private juce::MidiInputCallback
{
public:
    AudioIO();
    ~AudioIO() override;

    // An Engine's, made with its first engine context and kept while any is (model/engine/EngineContext.h); or, for code
    // that has the Engine alone, the one it has, if any.
    static std::shared_ptr<AudioIO> getFor (const te::Engine&);
    static AudioIO* findFor (const te::Engine&);

    juce::AudioDeviceManager& getDevices() noexcept { return devices; }

    // What Phazite's settings keep of them.
    struct Saved
    {
        std::unique_ptr<juce::XmlElement> audioSetup; // JUCE's device manager's state, or none yet (a first run)
        juce::StringArray midiInputsOff;              // by their IDs: every other MIDI input plays
        bool overdub = true, recordQuantize = false;  // the MIDI recordings' options (model/transport/internal/Recording.h)
        double recordingCorrection = 0.0;             // milliseconds (model/transport/internal/AudioRecorder.h)
    };

    // Opens the output and inputs as saved, or else the default output with no inputs (on a first run, Windows' Low Latency
    // Mode where it offers a small buffer: AudioDevices.h), and the MIDI inputs not switched off, and takes the options.
    // The app's, at its start; tests open a device of their own on getDevices().
    void open (const Saved&);

    // What to keep, after a change it told of. The audio setup is JUCE's, as chosen in the settings (not the Low Latency
    // switch's buffer).
    Saved getSaved() const;

    // The MIDI devices connected, each on or off, by their IDs; listed again as they are plugged in.
    struct MidiInput
    {
        juce::String id, name;
        bool isOn = false;
    };

    std::vector<MidiInput> getMidiInputs() const;
    void setMidiInputOn (const juce::String& id, bool); // an ID that is gone changes nothing

    bool isOverdubOn() const noexcept { return overdub; }
    void setOverdubOn (bool);
    bool isRecordQuantizeOn() const noexcept { return recordQuantize; }
    void setRecordQuantizeOn (bool);
    double getRecordingCorrection() const noexcept { return recordingCorrection; }
    void setRecordingCorrection (double milliseconds);

    // Where live MIDI goes: an engine and a track in it, by its raw ID, or none; set by the session of the project the
    // engine plays (model/playback/internal/PlaybackSession.h), and let go of by it before its engine goes.
    void setLiveMidi (engine::Engine*, std::int64_t track);
    void stopLiveMidi (const engine::Engine&); // if it goes to that engine

    // Plays a note-on or note-off into it as a MIDI keyboard would (the computer keyboard's, from the message thread; the
    // MIDI inputs', from their threads). Lock-free; other messages, and notes while it goes nowhere, do nothing.
    void playLive (const juce::MidiMessage&) noexcept;

private:
    juce::AudioDeviceManager devices;
    std::vector<std::unique_ptr<juce::MidiInput>> openMidiInputs; // those on, open
    juce::StringArray midiInputsOff;
    bool midiOpened = false; // once open() has opened them: tests open none
    juce::MidiDeviceListConnection midiDevicesChanged;
    bool overdub = true, recordQuantize = false;
    double recordingCorrection = 0.0;

    std::atomic<engine::Engine*> liveEngine { nullptr };
    std::atomic<std::int64_t> liveTrack { 0 };
    std::atomic<int> playingLive { 0 }; // the calls of playLive() under way, which stopLiveMidi() waits for

    void openMidiInputsOn(); // closes those switched off or gone, and opens those on
    void changeListenerCallback (juce::ChangeBroadcaster*) override { sendChangeMessage(); }
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override { playLive (message); }

    JUCE_DECLARE_NON_COPYABLE (AudioIO)
};

static_assert (std::atomic<std::int64_t>::is_always_lock_free);
} // namespace phazite
