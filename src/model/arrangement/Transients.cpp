// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Transients.h"
#include "devices/sampler/SampleAnalysis.h"
#include "internal/AudioWork.h"
#include "model/engine/AudioFiles.h"

#include <algorithm>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>

namespace phazite
{
Transients::Transients()
    : juce::Thread ("Phazite transients")
{
}

Transients::~Transients()
{
    signalThreadShouldExit();
    wake.signal();
    // A long file's analysis finishes first: never ended by force, which can leave a lock it held held for ever.
    stopThread (-1);
}

juce::String Transients::getKey (const juce::File& file)
{
    return file.getFullPathName() + "|" + juce::String (file.getSize()) + "|"
           + juce::String (file.getLastModificationTime().toMilliseconds());
}

std::optional<std::vector<double>> Transients::find (const juce::File& file)
{
    {
        const std::scoped_lock scoped (lock);

        if (const auto hit = found.find (getKey (file)); hit != found.end())
            return hit->second;

        // Once: not again while it waits, nor while it is being analysed.
        if (file != analysing && std::find (waiting.begin(), waiting.end(), file) == waiting.end())
            waiting.push_back (file);
    }

    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);

    wake.signal();
    return std::nullopt;
}

std::optional<std::vector<double>> Transients::lookUp (const juce::File& file)
{
    const std::scoped_lock scoped (lock);

    if (const auto hit = found.find (getKey (file)); hit != found.end())
        return hit->second;

    return std::nullopt;
}

void Transients::keep (const juce::File& file, const std::vector<double>& hits)
{
    const std::scoped_lock scoped (lock);
    std::erase (waiting, file);
    found[getKey (file)] = hits;
}

std::optional<std::vector<double>> Transients::analyse (const juce::File& file, const std::function<bool()>& shouldStop,
                                                        const std::function<void (float)>& progress, juce::int64* framesKept)
{
    std::vector<double> hits;
    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

    if (framesKept != nullptr)
        *framesKept = 0;

    if (reader == nullptr || ! (reader->sampleRate > 0.0) || reader->lengthInSamples <= 0 || reader->numChannels == 0
        || reader->numChannels > 64)
        return hits;

    // As much as longestSeconds, a second at a time, so that it can stop between them; its channels mixed as the onsets'
    // detection mixes them, and a rate over keptRate kept at a share of it (each so many frames averaged), so that what it
    // keeps is at most longestSeconds at keptRate, whatever the file says. It stops where the sound ends, the stream used up
    // and what comes silence the reader makes up: a header that says the file is longer than it is costs only what it
    // holds, as it claims, never more than that most.
    const auto every = std::max (1, (int) std::ceil (reader->sampleRate / keptRate));
    const auto rate = reader->sampleRate / every;
    const auto frames = std::min (reader->lengthInSamples, (juce::int64) (longestSeconds * reader->sampleRate));
    const auto channels = (int) reader->numChannels;
    const auto piece = juce::jlimit (4096, 1 << 20, (int) reader->sampleRate / every * every);
    const auto bytesPerFrame = (juce::int64) channels * std::max (1, (int) reader->bitsPerSample / 8);
    const auto share = 1.0f / (float) channels;
    juce::AudioBuffer<float> read (channels, piece);
    // What it will hold, at most, held at once, and written straight into: no growing, nothing copied after.
    const auto most = (int) std::min ((juce::int64) (keptRate * longestSeconds), frames / every + 1);
    juce::AudioBuffer<float> audio (1, std::max (1, most));
    auto* kept = audio.getWritePointer (0);
    auto count = 0;

    for (juce::int64 start = 0; start < frames; start += piece)
    {
        if (shouldStop != nullptr && shouldStop())
            return std::nullopt;

        const auto length = (int) std::min ((juce::int64) piece, frames - start);
        const auto usedUp = reader->input != nullptr && reader->input->isExhausted(); // before: what a decoder holds comes now

        if (! reader->read (&read, 0, length, start, true, true))
            read.clear (0, length);

        AudioWork::noteRead (length * bytesPerFrame);
        auto silent = true;

        for (int i = 0; i + every <= length; i += every)
        {
            auto sum = 0.0f;

            for (int k = 0; k < every; ++k)
            {
                auto mixed = 0.0f;

                for (int channel = 0; channel < channels; ++channel)
                    mixed += read.getSample (channel, i + k) * share;

                sum += mixed;
            }

            const auto sample = sum / (float) every;
            silent = silent && sample == 0.0f;

            if (count < most)
                kept[count++] = sample;
        }

        if (progress != nullptr)
            progress (0.5f * (float) ((double) (start + length) / (double) frames)); // then the hits are found

        if (silent && usedUp)
            break;
    }

    if (framesKept != nullptr)
        *framesKept = count;

    if (count == 0)
        return hits;

    audio.setSize (1, count, true, false, true); // what it read, kept where it is

    const auto onsets = devices::detectOnsets (audio, rate, shouldStop);

    if (shouldStop != nullptr && shouldStop())
        return std::nullopt;

    for (const auto& onset : onsets)
        hits.push_back ((double) onset.sample / rate);

    if (progress != nullptr)
        progress (1.0f);

    return hits;
}

void Transients::run()
{
    while (! threadShouldExit())
    {
        juce::File next;

        {
            const std::scoped_lock scoped (lock);

            if (! waiting.empty())
            {
                next = waiting.front();
                waiting.erase (waiting.begin());
            }

            analysing = next;
        }

        if (next == juce::File())
        {
            wake.wait (1000);
            continue;
        }

        auto hits = analyse (next, [this] { return threadShouldExit(); });
        ++analysed;
        const std::scoped_lock scoped (lock);

        if (hits.has_value())
            found[getKey (next)] = std::move (*hits);

        analysing = juce::File();
    }
}
} // namespace phazite
