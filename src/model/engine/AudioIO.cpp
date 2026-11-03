// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioIO.h"
#include "AudioDevices.h"
#include "engine/nodes/DeviceRunner.h"

#include <algorithm>
#include <map>
#include <thread>
#include <utility>

namespace phazite
{
namespace
{
    // Every output channel the device has, as Tracktion opened them; no input unless the setup chose one (Phazite opens no
    // microphone by itself: a track's input opens it, model/transport/internal/AudioRecorder.h).
    constexpr int allOutputs = 512;

    // Each Engine's, for what has the Engine alone (message thread).
    std::map<const te::Engine*, std::weak_ptr<AudioIO>>& getAll()
    {
        static std::map<const te::Engine*, std::weak_ptr<AudioIO>> all;
        return all;
    }
} // namespace

AudioIO::AudioIO()
    : midiDevicesChanged (juce::MidiDeviceListConnection::make (
          [this]
          {
              openMidiInputsOn();
              sendChangeMessage();
          }))
{
    devices.addChangeListener (this);
}

AudioIO::~AudioIO()
{
    devices.removeChangeListener (this);
    openMidiInputs.clear();
    devices.closeAudioDevice();
}

std::shared_ptr<AudioIO> AudioIO::getFor (const te::Engine& engine)
{
    auto& kept = getAll()[&engine];

    if (auto io = kept.lock())
        return io;

    auto io = std::make_shared<AudioIO>();
    kept = io;
    return io;
}

AudioIO* AudioIO::findFor (const te::Engine& engine)
{
    const auto found = getAll().find (&engine);
    return found != getAll().end() ? found->second.lock().get() : nullptr;
}

void AudioIO::open (const Saved& saved)
{
    JUCE_ASSERT_MESSAGE_THREAD
    overdub = saved.overdub;
    recordQuantize = saved.recordQuantize;
    recordingCorrection = saved.recordingCorrection;
    midiInputsOff = saved.midiInputsOff;

    const auto error = saved.audioSetup != nullptr ? devices.initialise (0, allOutputs, saved.audioSetup.get(), true)
                                                   : devices.initialiseWithDefaultDevices (0, allOutputs);

    if (error.isNotEmpty())
        juce::Logger::writeToLog ("Audio devices: " + error);

    // A first run: Windows' Low Latency Mode, where it offers a small buffer (docs/design.md, section 20).
    if (saved.audioSetup == nullptr)
        chooseLowLatencySetup (devices);

    midiOpened = true;
    openMidiInputsOn();
}

AudioIO::Saved AudioIO::getSaved() const
{
    Saved saved;
    saved.audioSetup = devices.createStateXml();
    saved.midiInputsOff = midiInputsOff;
    saved.overdub = overdub;
    saved.recordQuantize = recordQuantize;
    saved.recordingCorrection = recordingCorrection;
    return saved;
}

//==============================================================================
std::vector<AudioIO::MidiInput> AudioIO::getMidiInputs() const
{
    std::vector<MidiInput> inputs;

    for (const auto& device : juce::MidiInput::getAvailableDevices())
        inputs.push_back ({ device.identifier, device.name, ! midiInputsOff.contains (device.identifier) });

    return inputs;
}

void AudioIO::setMidiInputOn (const juce::String& id, bool on)
{
    const auto available = juce::MidiInput::getAvailableDevices();
    const auto known =
        std::any_of (available.begin(), available.end(), [&id] (const juce::MidiDeviceInfo& device) { return device.identifier == id; });

    if (! known || midiInputsOff.contains (id) != on)
        return;

    if (on)
        midiInputsOff.removeString (id);
    else
        midiInputsOff.add (id);

    openMidiInputsOn();
    sendChangeMessage();
}

void AudioIO::openMidiInputsOn()
{
    if (! midiOpened)
        return;

    const auto available = juce::MidiInput::getAvailableDevices();
    const auto isThere = [&available] (const juce::String& id)
    {
        return std::any_of (available.begin(), available.end(),
                            [&id] (const juce::MidiDeviceInfo& device) { return device.identifier == id; });
    };

    // Those switched off or unplugged close (each stops before it goes).
    std::erase_if (openMidiInputs, [&] (const std::unique_ptr<juce::MidiInput>& input)
                   { return midiInputsOff.contains (input->getIdentifier()) || ! isThere (input->getIdentifier()); });

    for (const auto& device : available)
    {
        const auto isOpen =
            std::any_of (openMidiInputs.begin(), openMidiInputs.end(),
                         [&device] (const std::unique_ptr<juce::MidiInput>& input) { return input->getIdentifier() == device.identifier; });

        if (isOpen || midiInputsOff.contains (device.identifier))
            continue;

        if (auto input = juce::MidiInput::openDevice (device.identifier, this))
        {
            input->start();
            openMidiInputs.push_back (std::move (input));
        }
        else
        {
            juce::Logger::writeToLog ("MIDI input didn't open: " + device.name);
        }
    }
}

//==============================================================================
void AudioIO::setOverdubOn (bool on)
{
    if (std::exchange (overdub, on) != on)
        sendChangeMessage();
}

void AudioIO::setRecordQuantizeOn (bool on)
{
    if (std::exchange (recordQuantize, on) != on)
        sendChangeMessage();
}

void AudioIO::setRecordingCorrection (double milliseconds)
{
    if (! juce::exactlyEqual (std::exchange (recordingCorrection, milliseconds), milliseconds))
        sendChangeMessage();
}

//==============================================================================
void AudioIO::setLiveMidi (engine::Engine* engine, std::int64_t track)
{
    liveTrack.store (track);
    liveEngine.store (engine);
}

void AudioIO::stopLiveMidi (const engine::Engine& engine)
{
    auto* expected = const_cast<engine::Engine*> (&engine);
    liveEngine.compare_exchange_strong (expected, nullptr);

    // A call that took the engine before finishes before the engine may go: the count goes up before the engine is read.
    while (playingLive.load() > 0)
        std::this_thread::yield();
}

void AudioIO::playLive (const juce::MidiMessage& message) noexcept
{
    playingLive.fetch_add (1);

    if (auto* engine = liveEngine.load())
        if (const auto event = engine::toNoteEvent (message, 0); event.has_value())
            engine->playLive (liveTrack.load(), *event);

    playingLive.fetch_sub (1);
}
} // namespace phazite
