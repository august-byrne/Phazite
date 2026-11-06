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

    // How its blocks keep time: as an audio device's do, a plug-in's block in its helper silent once its deadline passes (for
    // what a late or hanging plug-in does); or as a render's, each waiting for what it plays, so that what plug-ins in helpers
    // play is the same however busy the machine is (known bug 7 until 2026-11-06: a block late under load lowered a level).
    enum class Timing
    {
        realTime,
        asRender
    };

    explicit EnginePlayback (Input in = Input::silence, Timing timing = Timing::realTime)
        : input (in)
    {
        auto& host = getPlaybackHost (getEngineContext());
        host.prepare (sampleRate, blockSize, 2, input == Input::loopback ? blockSize : 0);
        host.getEngine().setPlayingAsRender (timing == Timing::asRender);
        previous.clear();
        nextInput();
    }

    // Stopped, keeping to real time, and on a device without inputs again.
    ~EnginePlayback()
    {
        auto& host = getPlaybackHost (getEngineContext());
        host.getEngine().stop();
        host.getEngine().setPlayingAsRender (false);
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

    // Waits until the audio files the engine's plan plays have been read (its audio clips', and a version's bounce compared
    // with the song: a short file whole, a longer one's start), the song's changes given to it first. Live, the engine's
    // reading thread reads them ahead of the audio thread (engine/audio/AudioFile.h), and played faster than real time, what
    // isn't read yet would sound as silence.
    static void waitForFiles()
    {
        dispatchPendingMessages();
        const auto isRead = [] (const std::shared_ptr<engine::AudioFile>& file)
        {
            if (file == nullptr)
                return true;

            const auto whole = (double) file->getLength() <= engine::AudioFile::shortSeconds * file->getSampleRate();
            const auto end = whole ? file->getLength() : std::min<std::int64_t> (file->getLength(), engine::AudioFile::chunkSamples);

            for (std::int64_t at = 0; at < end; at += engine::AudioFile::chunkSamples)
                if (auto count = 1; file->getSamples (0, at, count) == nullptr)
                    return false;

            return true;
        };

        REQUIRE (dispatchMessagesUntil (
            [&isRead]
            {
                const auto* plan = getPlaybackHost (getEngineContext()).getEngine().getPlan();

                if (plan == nullptr)
                    return false;

                for (const auto& strip : plan->strips)
                    for (const auto& region : strip.audio)
                        if (! isRead (region.file))
                            return false;

                return ! plan->comparison.has_value() || isRead (plan->comparison->bounce.file);
            }));
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
