// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own engine playing the open project as the app plays it (model/playback/),
// driven by a test a block at a time, as an audio device would drive it, but not in real time: what it plays is the same
// every time. For as long as it lasts, the app's engine plays on a device with two inputs.
#pragma once

#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineContext.h"
#include "model/playback/internal/PlaybackHost.h"

#include <cmath>

namespace phazite::test
{
class EnginePlayback
{
public:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;

    // What comes into the device's two inputs: silence; what it played a block before, as a cable from an audio
    // interface's outputs to its inputs would bring it back (for recording audio), which the device reports as its
    // inputs' delay; or a tone, 440 Hz at -12 dBFS.
    enum class Input
    {
        silence,
        loopback,
        tone
    };

    explicit EnginePlayback (Input in = Input::silence)
        : input (in)
    {
        getPlaybackHost (getEngineContext()).prepare (sampleRate, blockSize, 2, input == Input::loopback ? blockSize : 0);
        previous.clear();
        nextInput();
    }

    // Stopped, and on a device without inputs again.
    ~EnginePlayback()
    {
        auto& host = getPlaybackHost (getEngineContext());
        host.getEngine().stop();
        host.prepare (sampleRate, blockSize);
    }

    // Plays so many seconds of what the engine plays, the song's changes given to it first (dispatchPendingMessages()).
    juce::AudioBuffer<float> play (double seconds)
    {
        dispatchPendingMessages();
        auto& engine = getPlaybackHost (getEngineContext()).getEngine();
        const auto numSamples = (int) std::lround (seconds * sampleRate);
        juce::AudioBuffer<float> audio (2, numSamples);

        for (int done = 0; done < numSamples; done += blockSize)
        {
            const auto length = std::min (blockSize, numSamples - done);
            const float* inputs[] = { previous.getReadPointer (0), previous.getReadPointer (1) };
            float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
            engine.process (inputs, 2, channels, 2, length);

            if (input == Input::loopback)
                for (int channel = 0; channel < 2; ++channel)
                    previous.copyFrom (channel, 0, audio, channel, done, length);

            nextInput();
        }

        return audio;
    }

private:
    const Input input;
    juce::AudioBuffer<float> previous { 2, blockSize }; // what comes in
    std::int64_t toneSample = 0;

    // The tone's next block.
    void nextInput()
    {
        if (input == Input::tone)
            for (int i = 0; i < blockSize; ++i, ++toneSample)
                for (int channel = 0; channel < 2; ++channel)
                    previous.setSample (
                        channel, i,
                        0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * (double) toneSample / sampleRate));
    }

    EnginePlayback (const EnginePlayback&) = delete;
    EnginePlayback& operator= (const EnginePlayback&) = delete;
};
} // namespace phazite::test
