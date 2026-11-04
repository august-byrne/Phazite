// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own engine on the audio device: one for the app, which its EngineContext owns, a callback on the device
// Phazite opens (model/engine/internal/AudioIO.h), which plays the song of the open project's PlaybackSession, with
// what comes into the device's inputs, which the tracks that hear them play and a take records; and the browser's
// previews beside it.
#pragma once

#include "engine/audio/FileReader.h"
#include "engine/core/Engine.h"
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <deque>
#include <optional>
#include <vector>

namespace phazite
{
class AudioIO;
class EngineContext;
class PlaybackSession;
class PluginHost;

class PlaybackHost : private juce::AudioIODeviceCallback,
                     private juce::Timer // which takes in what was played live, and logs blocks that came late
{
public:
    // On the audio device Phazite opens (model/engine/internal/AudioIO.h), with the plug-ins hosted in helpers, on so many
    // threads, the audio device's among them.
    PlaybackHost (AudioIO&, PluginHost&, int numThreads);
    ~PlaybackHost() override;

    AudioIO& getAudioIO() const noexcept { return audio; }
    PluginHost& getPluginHost() const noexcept { return plugins; }
    int getNumThreads() const noexcept { return numThreads; } // for renders, which play on as many

    engine::Engine& getEngine() noexcept { return engine; }
    engine::FileReader& getFileReader() noexcept { return fileReader; } // the audio files the engine plays, read ahead

    // The browser's previews (model/engine/Preview.cpp): an engine of their own, which plays beside the song's, its
    // sound added to the song's after the master, prepared as the song's is; and a reading thread of its own for what it
    // plays, where a loop stretched as it starts waits for its render, never holding up the song's files.
    engine::Engine& getPreviewEngine() noexcept { return previews; }
    engine::FileReader& getPreviewFileReader() noexcept { return previewFiles; }

    // The audio thread, or a test as a device would: plays a block of the song's engine with the device's open inputs (by
    // their channels), and the previews' added to it.
    void process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept;

    // The sessions of the projects open (model/playback/internal/PlaybackSession.h): the one added last plays, and the one
    // before it again when it goes (a test's second project). As one goes, what the engine played of it goes too (plug-ins
    // among it, which the session's PluginInstances own), once the audio thread can't be playing it; and so do the
    // notes played into it, which Capture takes no more.
    void add (PlaybackSession&);
    void remove (PlaybackSession&);
    PlaybackSession* getPlayingSession() const noexcept;

    // Gives the engine no plan, and lets go of what it played once the audio thread can't be playing it (a render that
    // plays the project's plug-ins has them alone then: PlaybackSession::lendToRender()).
    void dropPlan() { letGoOfPlans(); }

    // Prepares the engine as a device would (the tests' devices, which aren't JUCE's, with so many inputs, whose sound comes in
    // so late), and gives it the playing session's song for that rate.
    void prepare (double sampleRate, int maximumBlockSize, int deviceInputs = 0, int deviceInputLatency = 0);

    // The audio device's inputs, as many as it has (open or not), and how late what comes in through them is, and what
    // goes out to be heard, in samples, as its driver reports them.
    int getNumInputs() const noexcept { return numInputs.load(); }
    int getInputLatency() const noexcept { return inputLatency.load(); }
    int getOutputLatency() const noexcept { return outputLatency.load(); }

    // Low Latency (docs/design.md, section 20): while it is on, what is played live has no delay compensation. Phazite's
    // devices that look ahead by more than lowLatencyLimitSeconds let the sound through as it comes and report no latency,
    // and the output's buffer shrinks to chooseLowLatencyBufferSize()'s (model/engine/AudioDevices.h) where it offers one
    // smaller. Turning it off puts the buffer back, unless it was changed meanwhile. Nothing is saved: the setup saved stays
    // the one chosen in the settings. The playing session takes it in with its next plan, without stopping.
    bool isLowLatency() const noexcept { return lowLatency; }
    void setLowLatency (bool);

    // The recordable notes played live in the last 30 seconds by the engine's clock (Engine::takePlayed()), oldest first:
    // what Capture and a take make clips of. Message thread.
    const std::deque<engine::Engine::PlayedEvent>& getPlayedEvents();
    void keepPlayedFrom (std::optional<std::int64_t> clock) { keepFrom = clock; } // older ones too, while a take records

private:
    AudioIO& audio;
    PluginHost& plugins;
    juce::AudioDeviceManager& devices;
    const int numThreads;
    bool lowLatency = false;
    int bufferBeforeLowLatency = 0, lowLatencyBuffer = 0; // the output's buffer before Low Latency, and the one it chose
    engine::Engine engine;
    engine::FileReader fileReader;
    engine::Engine previews;
    engine::FileReader previewFiles;
    juce::AudioBuffer<float> previewBlock; // the audio thread's, made as the engines are prepared
    void prepareEngines (double sampleRate, int maximumBlockSize);
    std::vector<PlaybackSession*> sessions;
    std::atomic<bool> running { false }; // a device calls the engine from its own thread
    void letGoOfPlans();
    void forgetPlayed();

    std::atomic<int> numInputs { 0 }, inputLatency { 0 }, outputLatency { 0 };
    std::vector<int> openInputs;                                       // each open input's channel, as the device hands them
    std::array<const float*, engine::Engine::maximumInputs> inputs {}; // the audio thread's: by their channels
    std::deque<engine::Engine::PlayedEvent> played;
    std::optional<std::int64_t> keepFrom;
    std::vector<engine::Engine::PlayedEvent> taking;
    void timerCallback() override;

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override { running = false; }

    JUCE_DECLARE_WEAK_REFERENCEABLE (PlaybackHost)
    JUCE_DECLARE_NON_COPYABLE (PlaybackHost)
};

// How far a device may look ahead while Low Latency is on, in seconds.
inline constexpr double lowLatencyLimitSeconds = 0.001;

} // namespace phazite
