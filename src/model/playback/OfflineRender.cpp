// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/OfflineRender.h"
#include "engine/core/Engine.h"
#include "internal/PlaybackSession.h"

#include <algorithm>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>

namespace phazite
{
namespace
{
    constexpr float silence = 0.000316f; // -70 dB, under which a tail's block is silent
    constexpr int partSamples = 8192;    // rendered and written at a time

    std::unique_ptr<juce::AudioFormatWriter> makeFloatWriter (const juce::File& file, double sampleRate)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
            return nullptr;

        return juce::WavAudioFormat().createWriterFor (stream,
                                                       juce::AudioFormatWriterOptions {}
                                                           .withSampleRate (sampleRate)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    }
} // namespace

OfflineRender::OfflineRender (Setup s, const std::vector<Job>& toRender, Rendered whenRendered, Finished whenFinished)
    : juce::Thread ("Phazite render"),
      setup (std::move (s)),
      rendered (std::move (whenRendered)),
      finished (std::move (whenFinished))
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The project's engine plays none of its plug-ins while this has them.
    if (setup.session != nullptr)
        setup.session->lendToRender (true);

    for (const auto& job : toRender)
    {
        Compiled compiled { job, setup.makePlayer(), nullptr };
        compiled.plan = compiled.player->compile (setup.sampleRate, setup.blockSize, std::nullopt, job.of);
        jobs.push_back (std::move (compiled));
    }

    startThread (setup.background ? juce::Thread::Priority::low : juce::Thread::Priority::normal);
}

OfflineRender::~OfflineRender()
{
    JUCE_ASSERT_MESSAGE_THREAD
    // It looks at threadShouldExit() between blocks: never ended by force, which can leave a lock it held held for ever.
    stopThread (-1);
    jobs.clear(); // what the renders played, before the project's engine plays the plug-ins again

    if (setup.session != nullptr)
        setup.session->lendToRender (false);
}
void OfflineRender::setProgress (int job, double ofJob) noexcept
{
    const auto share = juce::jlimit (0.0, 1.0, ofJob);
    progress.store ((float) ((job + share) / (double) std::max<size_t> (1, jobs.size())), std::memory_order_relaxed);
}

void OfflineRender::run()
{
    auto result = juce::Result::ok();

    for (size_t i = 0; i < jobs.size() && result.wasOk() && ! threadShouldExit(); ++i)
        result = play (jobs[i], (int) i);

    if (! threadShouldExit() && finished != nullptr)
        finished (result);
}

juce::Result OfflineRender::play (Compiled& compiled, int index)
{
    const auto& job = compiled.job;
    const auto failed = juce::Result::fail ("Phazite couldn't write " + job.file.getFileName() + ".");
    auto writer = makeFloatWriter (job.file, setup.sampleRate);

    if (writer == nullptr)
        return failed;

    // From the song's time it starts at: its samples from there are what it counts.
    const auto firstSample = (std::int64_t) std::llround (std::max (0.0, job.startSeconds) * setup.sampleRate);
    const auto firstBeat = compiled.plan->tempo.toBeats ((double) firstSample);

    engine::Engine engine;
    engine.setNumThreads (setup.numThreads);
    engine.prepare (setup.sampleRate, setup.blockSize);
    engine.setPlan (std::move (compiled.plan));
    engine.locate (firstBeat);

    engine::LoudnessMeter meter;
    meter.prepare (setup.sampleRate, partSamples);
    juce::AudioBuffer<float> part (2, partSamples);
    const auto endSample = std::max<std::int64_t> (0, (std::int64_t) std::llround (job.endSeconds * setup.sampleRate) - firstSample);
    const auto lastSample = endSample + (std::int64_t) std::llround (job.tailSeconds * setup.sampleRate);
    std::int64_t done = 0;
    engine.beginRender();

    // The song, then its tails a block at a time, until one falls silent.
    while (! threadShouldExit() && done < lastSample)
    {
        const auto inSong = done < endSample;
        const auto length = inSong ? (int) std::min<std::int64_t> (partSamples, endSample - done) : setup.blockSize;
        engine.renderPart (part.getArrayOfWritePointers(), length);

        if (! inSong && part.getMagnitude (0, length) < silence)
            break;

        if (! writer->writeFromAudioSampleBuffer (part, 0, length))
            return failed;

        meter.process (part.getArrayOfReadPointers(), 2, length);
        done += length;
        setProgress (index, setup.playedShare * (double) done / (double) std::max<std::int64_t> (1, endSample));
    }

    engine.endRender();
    writer = nullptr; // written whole

    if (threadShouldExit())
        return juce::Result::fail ("Cancelled.");

    return rendered != nullptr ? rendered (*this, index, meter.getReadings()) : juce::Result::ok();
}
} // namespace phazite
