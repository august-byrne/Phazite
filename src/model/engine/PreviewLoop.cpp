// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PreviewLoop.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // How much of the loop's end the render hears before its start, and of its start after its end, so that the stretched
    // loop goes round as smoothly as the loop does: a little more than Rubber Band's window.
    constexpr double wrapSeconds = 0.1;

    // How long the reading thread waits for a part of the loop the render hasn't reached: past it, the part plays as
    // silence. The render keeps ahead of what plays once the loop is ready to play (isReadyToPlay()).
    constexpr int waitMilliseconds = 5000;

    class LoopReader final : public engine::SampleReader
    {
    public:
        explicit LoopReader (std::shared_ptr<const StretchedLoop> stretched)
            : loop (std::move (stretched))
        {
        }

        int getNumChannels() const override { return loop->getNumChannels(); }
        double getSampleRate() const override { return loop->getSampleRate(); }
        std::int64_t getLength() const override { return loop->getLength(); }

        bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
        {
            if (! loop->waitUntilRendered (start + numSamples, waitMilliseconds))
                return false;

            loop->read (start, buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples);
            return true;
        }

    private:
        std::shared_ptr<const StretchedLoop> loop;
    };
} // namespace

StretchedLoop::StretchedLoop (te::Engine& engine, const juce::File& file, double timeRatio, double rate)
    : juce::Thread ("Phazite preview stretch"),
      reader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file)),
      sampleRate (rate)
{
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels == 0 || rate <= 0.0
        || timeRatio <= 0.0)
        return;

    settings.timeRatio = timeRatio;
    settings.rateRatio = rate / reader->sampleRate;
    length = stretch::getStretchedLength (reader->lengthInSamples, settings);
    audio.setSize ((int) std::min (reader->numChannels, 2u), (int) length);
    audio.clear();
    startThread (juce::Thread::Priority::high);
}

StretchedLoop::~StretchedLoop()
{
    stopThread (10000);
}

bool StretchedLoop::isComplete() const noexcept
{
    return ready.load (std::memory_order_acquire) >= length;
}

bool StretchedLoop::isReadyToPlay() const
{
    if (isComplete())
        return true;

    const auto rendered = ready.load (std::memory_order_acquire);
    const auto spentSeconds = (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0;
    return rendered >= engine::AudioFile::chunkSamples && (double) rendered / sampleRate >= 2.0 * spentSeconds;
}

void StretchedLoop::read (juce::int64 position, float* const* channels, int numChannels, int numFrames) const noexcept
{
    for (int channel = 0; channel < numChannels; ++channel)
        juce::FloatVectorOperations::clear (channels[channel], numFrames);

    // The part of the block the loop has ready.
    const auto from = std::max (position, (juce::int64) 0);
    const auto to = std::min (position + numFrames, ready.load (std::memory_order_acquire));

    if (to <= from)
        return;

    for (int channel = 0; channel < numChannels; ++channel)
        juce::FloatVectorOperations::copy (channels[channel] + (from - position),
                                           audio.getReadPointer (std::min (channel, audio.getNumChannels() - 1), (int) from),
                                           (int) (to - from));
}

void StretchedLoop::run()
{
    // The loop with a little of its end before it and of its start after it, read from its file as the render goes, of
    // which the render keeps the loop's part.
    const auto loopLength = reader->lengthInSamples;
    const auto wrap = std::min (loopLength, (juce::int64) std::llround (wrapSeconds * reader->sampleRate));
    stretch::Source source;
    source.numChannels = audio.getNumChannels();
    source.sampleRate = reader->sampleRate;
    source.length = loopLength + 2 * wrap;
    source.read = [this, wrap, loopLength] (juce::AudioBuffer<float>& buffer, juce::int64 start, int numFrames)
    {
        for (int done = 0; done < numFrames;)
        {
            const auto at = ((start + done - wrap) % loopLength + loopLength) % loopLength;
            const auto frames = (int) std::min ((juce::int64) (numFrames - done), loopLength - at);
            reader->read (&buffer, done, frames, at, true, buffer.getNumChannels() > 1);
            done += frames;
        }
    };

    const auto skip = stretch::getStretchedLength (wrap, settings);
    juce::int64 given = 0;

    stretch::render (
        source, settings,
        [this, skip, &given] (const juce::AudioBuffer<float>& block, int numFrames)
        {
            // The frames of the block that are the loop's.
            const auto from = std::max (given, skip), to = std::min (given + numFrames, skip + length);

            for (int channel = 0; channel < audio.getNumChannels() && to > from; ++channel)
                audio.copyFrom (channel, (int) (from - skip), block, channel, (int) (from - given), (int) (to - from));

            given += numFrames;
            ready.store (std::clamp (given - skip, (juce::int64) 0, length), std::memory_order_release);
            return given - skip < length;
        },
        [this] { return threadShouldExit(); });

    // What Rubber Band left short is silence, as the buffer is.
    if (! threadShouldExit())
        ready.store (length, std::memory_order_release);
}

bool StretchedLoop::waitUntilRendered (juce::int64 end, int timeoutMilliseconds) const
{
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMilliseconds;

    while (ready.load (std::memory_order_acquire) < std::min (end, length))
    {
        if (! isThreadRunning() || juce::Time::getMillisecondCounter() >= until)
            return ready.load (std::memory_order_acquire) >= std::min (end, length);

        juce::Thread::sleep (2);
    }

    return true;
}

std::unique_ptr<engine::SampleReader> makeLoopReader (std::shared_ptr<const StretchedLoop> loop)
{
    return std::make_unique<LoopReader> (std::move (loop));
}
} // namespace phazite
