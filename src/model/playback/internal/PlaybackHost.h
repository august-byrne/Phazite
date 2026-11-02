// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own engine on the audio device (docs/tracktion-migration.md, T2): one for the app, which its EngineContext
// owns. While T2 builds it, it plays beside Tracktion's when the app says so (setOn()): a second callback on the device
// Tracktion opens (JUCE adds what the two play), which plays the song of the open project's PlaybackSession, while
// Tracktion's plays only what Phazite's doesn't yet (live notes, recording).
#pragma once

#include "engine/core/Engine.h"
#include "model/base/internal/Tracktion.h"

#include <vector>

namespace phazite
{
class EngineContext;
class PlaybackSession;

class PlaybackHost : private juce::AudioIODeviceCallback
{
public:
    explicit PlaybackHost (te::Engine&);
    ~PlaybackHost() override;

    // Whether Phazite's engine plays the open project rather than Tracktion's. Off at first.
    bool isOn() const noexcept { return on; }
    void setOn (bool);

    engine::Engine& getEngine() noexcept { return engine; }

    // The sessions of the projects open (model/playback/internal/PlaybackSession.h): the one added last plays, and the one
    // before it again when it goes (a test's second project).
    void add (PlaybackSession&);
    void remove (PlaybackSession&);
    PlaybackSession* getPlayingSession() const noexcept;

    // Prepares the engine as a device would (the tests' devices, which aren't JUCE's), and gives it the playing session's
    // song for that rate.
    void prepare (double sampleRate, int maximumBlockSize);

private:
    te::Engine& tracktion;
    engine::Engine engine;
    bool on = false;
    std::vector<PlaybackSession*> sessions;

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}

    JUCE_DECLARE_WEAK_REFERENCEABLE (PlaybackHost)
    JUCE_DECLARE_NON_COPYABLE (PlaybackHost)
};

// The app's (EngineContext.h).
PlaybackHost& getPlaybackHost (EngineContext&);
} // namespace phazite
