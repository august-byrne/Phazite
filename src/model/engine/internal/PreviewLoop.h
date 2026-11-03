// SPDX-License-Identifier: AGPL-3.0-or-later
// A loop the browser previews in time with the song, stretched to the song's tempo as clips are: by Rubber Band's R3
// engine (devices/stretch/Stretch.h), on a thread of its own, into memory, at the output's rate, from the moment it is
// chosen; and Phazite's engine reading it as it reads a file (engine/audio/AudioFile.h), on the previews' reading thread,
// which waits there for what isn't rendered yet (model/playback/internal/PlaybackHost.h). For the previewer
// (model/engine/Preview.cpp) and tests.
#pragma once

#include "devices/stretch/Stretch.h"
#include "engine/audio/AudioFile.h"
#include "model/base/internal/Tracktion.h"

#include <atomic>
#include <memory>

namespace phazite
{
// A loop stretched to a tempo, rendered in the background from the moment it is made.
class StretchedLoop : private juce::Thread
{
public:
    // Renders the file so many times as long, for playing at the sample rate. Opens it on the calling thread: isValid()
    // says whether it could.
    StretchedLoop (const juce::File&, double timeRatio, double sampleRate);
    ~StretchedLoop() override; // stops the render, if it is still going

    bool isValid() const noexcept { return length > 0; }
    double getSampleRate() const noexcept { return sampleRate; }
    juce::int64 getLength() const noexcept { return length; } // in frames, at the rate it plays at
    int getNumChannels() const noexcept { return audio.getNumChannels(); }

    // Whether it can play from its start without the playback catching up with the render: all of it is ready, or as much
    // as the engine reads of a file at once (engine::AudioFile::chunkSamples) and the render runs at least twice as fast
    // as it plays.
    bool isReadyToPlay() const;
    bool isComplete() const noexcept;

    // Its frames from a position on into the channels (a mono loop into each), and silence before its start, after its
    // end, and where the render hasn't reached yet. Real-time safe.
    void read (juce::int64 position, float* const* channels, int numChannels, int numFrames) const noexcept;

    // Waits, on a thread that may wait (never the audio thread), until its frames before an end are rendered, the render
    // has stopped, or the time is up; whether they are.
    bool waitUntilRendered (juce::int64 end, int timeoutMilliseconds) const;

private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    stretch::Settings settings;
    double sampleRate = 0.0;
    juce::int64 length = 0;
    juce::AudioBuffer<float> audio;       // all of it, at sampleRate, allocated before the render starts
    std::atomic<juce::int64> ready { 0 }; // the frames rendered, from its start
    const double startedAt = juce::Time::getMillisecondCounterHiRes();

    void run() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StretchedLoop)
};

// The loop as Phazite's engine reads a file: each part once it is rendered, which the reading thread waits for.
std::unique_ptr<engine::SampleReader> makeLoopReader (std::shared_ptr<const StretchedLoop>);
} // namespace phazite
