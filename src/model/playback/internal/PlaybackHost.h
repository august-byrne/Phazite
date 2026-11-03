// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own engine on the audio device (docs/tracktion-migration.md, T2): one for the app, which its EngineContext
// owns, a second callback on the device Tracktion opens (JUCE adds what the two play: Tracktion's plays nothing of a
// project, PlaybackSession.h), which plays the song of the open project's PlaybackSession, with what comes into the
// device's inputs, which the tracks that hear them play and a take records.
#pragma once

#include "engine/audio/FileReader.h"
#include "engine/core/Engine.h"
#include "model/base/internal/Tracktion.h"

#include <array>
#include <atomic>
#include <deque>
#include <optional>
#include <vector>

namespace phazite
{
class EngineContext;
class PlaybackSession;

class PlaybackHost : private juce::AudioIODeviceCallback,
                     private juce::Timer // which takes in what was played live, and logs blocks that came late
{
public:
    explicit PlaybackHost (te::Engine&);
    ~PlaybackHost() override;

    engine::Engine& getEngine() noexcept { return engine; }
    engine::FileReader& getFileReader() noexcept { return fileReader; } // the audio files the engine plays, read ahead

    // The sessions of the projects open (model/playback/internal/PlaybackSession.h): the one added last plays, and the one
    // before it again when it goes (a test's second project). As one goes, what the engine played of it goes too (plug-ins
    // among it, which the project's Edit owns), once the audio thread can't be playing it; and so do the notes played into
    // it, which Capture takes no more.
    void add (PlaybackSession&);
    void remove (PlaybackSession&);
    PlaybackSession* getPlayingSession() const noexcept;

    // Prepares the engine as a device would (the tests' devices, which aren't JUCE's, with so many inputs, whose sound comes in
    // so late), and gives it the playing session's song for that rate.
    void prepare (double sampleRate, int maximumBlockSize, int deviceInputs = 0, int deviceInputLatency = 0);

    // The audio device's inputs, as many as it has (open or not), and how late what comes in through them is, and what
    // goes out to be heard, in samples, as its driver reports them.
    int getNumInputs() const noexcept { return numInputs.load(); }
    int getInputLatency() const noexcept { return inputLatency.load(); }
    int getOutputLatency() const noexcept { return outputLatency.load(); }

    // The recordable notes played live in the last 30 seconds by the engine's clock (Engine::takePlayed()), oldest first:
    // what Capture and a take make clips of. Message thread.
    const std::deque<engine::Engine::PlayedEvent>& getPlayedEvents();
    void keepPlayedFrom (std::optional<std::int64_t> clock) { keepFrom = clock; } // older ones too, while a take records

private:
    te::Engine& tracktion;
    engine::Engine engine;
    engine::FileReader fileReader;
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

// The app's (EngineContext.h).
PlaybackHost& getPlaybackHost (EngineContext&);
} // namespace phazite
