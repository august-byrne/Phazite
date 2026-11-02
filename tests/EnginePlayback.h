// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own engine playing the open project as the app plays it (model/playback/; docs/tracktion-migration.md, T2),
// driven by a test a block at a time, as an audio device would drive it, but not in real time: what it plays is the same
// every time. For as long as it lasts, the app's engine is Phazite's.
#pragma once

#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/EngineContext.h"
#include "model/playback/internal/PlaybackHost.h"

#include <cmath>

namespace phazite::test
{
class EnginePlayback
{
public:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;

    EnginePlayback()
    {
        getEngineContext().setPhaziteEngineOn (true);
        getPlaybackHost (getEngineContext()).prepare (sampleRate, blockSize);
    }

    ~EnginePlayback() { getEngineContext().setPhaziteEngineOn (false); }

    // Plays so many seconds of what the engine plays, the song's changes given to it first (dispatchPendingMessages()).
    juce::AudioBuffer<float> play (double seconds)
    {
        dispatchPendingMessages();
        auto& engine = getPlaybackHost (getEngineContext()).getEngine();
        const auto numSamples = (int) std::lround (seconds * sampleRate);
        juce::AudioBuffer<float> audio (2, numSamples);

        for (int done = 0; done < numSamples; done += blockSize)
        {
            float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
            engine.process (channels, 2, std::min (blockSize, numSamples - done));
        }

        return audio;
    }

private:
    EnginePlayback (const EnginePlayback&) = delete;
    EnginePlayback& operator= (const EnginePlayback&) = delete;
};
} // namespace phazite::test
