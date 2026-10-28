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

    // The loop Tracktion's preview Edit would stretch as it plays (its auto-tempo), on its first track, if it has one.
    te::WaveAudioClip* findStretchedClip (te::Edit& edit)
    {
        if (const auto tracks = te::getAudioTracks (edit); ! tracks.isEmpty())
            for (auto* clip : tracks.getFirst()->getClips())
                if (auto* wave = dynamic_cast<te::WaveAudioClip*> (clip); wave != nullptr && wave->getAutoTempo())
                    return wave;

        return nullptr;
    }

    // The player of stretched loops: on a track of its own, after the three of Tracktion's preview Edit, which keeps it
    // from one sound to the next.
    constexpr int loopTrack = 3;

    PreviewLoopPlugin* findLoopPlayer (te::Edit& edit)
    {
        if (const auto tracks = te::getAudioTracks (edit); tracks.size() > loopTrack)
            for (auto* plugin : tracks[loopTrack]->pluginList)
                if (auto* player = dynamic_cast<PreviewLoopPlugin*> (plugin))
                    return player;

        return nullptr;
    }

    PreviewLoopPlugin* addLoopPlayer (te::Edit& edit)
    {
        if (auto* player = findLoopPlayer (edit))
            return player;

        edit.ensureNumberOfAudioTracks (loopTrack + 1);
        te::getAudioTracks (edit)[loopTrack]->pluginList.insertPlugin (
            edit.getPluginCache().createNewPlugin (PreviewLoopPlugin::xmlTypeName, {}), 0, nullptr);
        return findLoopPlayer (edit);
    }
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

    const auto renderedSeconds = (double) ready.load (std::memory_order_acquire) / sampleRate;
    const auto spentSeconds = (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0;
    return renderedSeconds >= 0.25 && renderedSeconds >= 2.0 * spentSeconds;
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

//==============================================================================
const char* PreviewLoopPlugin::xmlTypeName = "phazitePreviewLoop";

PreviewLoopPlugin::PreviewLoopPlugin (te::PluginCreationInfo info)
    : te::Plugin (info)
{
}

PreviewLoopPlugin::~PreviewLoopPlugin()
{
    notifyListenersOfDeletion();
}

void PreviewLoopPlugin::setLoop (const StretchedLoop* newLoop) noexcept
{
    loop.store (newLoop, std::memory_order_release);
}

void PreviewLoopPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
}

void PreviewLoopPlugin::applyToBuffer (const te::PluginRenderContext& context)
{
    if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
        return;

    auto& buffer = *context.destBuffer;
    const auto* playing = loop.load (std::memory_order_acquire);

    // Rendered for the rate it plays at; not while the preview is stopped.
    if (playing == nullptr || ! context.isPlaying || ! juce::exactlyEqual (playing->getSampleRate(), sampleRate))
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.clear (channel, context.bufferStartSample, context.bufferNumSamples);

        return;
    }

    float* channels[2] {};
    const auto numChannels = std::min (buffer.getNumChannels(), 2);

    for (int channel = 0; channel < numChannels; ++channel)
        channels[channel] = buffer.getWritePointer (channel, context.bufferStartSample);

    playing->read ((juce::int64) std::llround (context.editTime.getStart().inSeconds() * sampleRate), channels, numChannels,
                   context.bufferNumSamples);
}

//==============================================================================
std::unique_ptr<StretchedLoop> stretchPreviewLoop (te::Edit& preview, const juce::File& file)
{
    auto* clip = findStretchedClip (preview);
    const auto fileSeconds = te::AudioFile (preview.engine, file).getLength();

    if (clip == nullptr || fileSeconds <= 0.0)
        return {};

    // As long as Tracktion made the clip: the loop's beats at the song's tempo.
    auto loop = std::make_unique<StretchedLoop> (preview.engine, file, clip->getPosition().getLength().inSeconds() / fileSeconds,
                                                 preview.engine.getDeviceManager().getSampleRate());
    auto* player = loop->isValid() ? addLoopPlayer (preview) : nullptr;

    if (player == nullptr)
        return {};

    clip->disabled = true;
    player->setLoop (loop.get());
    return loop;
}

void clearPreviewLoop (te::Edit& preview)
{
    if (auto* player = findLoopPlayer (preview))
        player->setLoop (nullptr);
}
} // namespace phazite
