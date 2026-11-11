// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Transients.h"
#include "devices/sampler/SampleAnalysis.h"
#include "model/engine/AudioFiles.h"

#include <algorithm>
#include <juce_audio_formats/juce_audio_formats.h>

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

        if (std::find (waiting.begin(), waiting.end(), file) == waiting.end())
            waiting.push_back (file);
    }

    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);

    wake.signal();
    return std::nullopt;
}

std::vector<double> Transients::findNow (const juce::File& file)
{
    const auto key = getKey (file);

    {
        const std::scoped_lock scoped (lock);

        if (const auto hit = found.find (key); hit != found.end())
            return hit->second;
    }

    auto hits = analyse (file);
    const std::scoped_lock scoped (lock);
    std::erase (waiting, file);
    found[key] = hits;
    return hits;
}

std::vector<double> Transients::analyse (const juce::File& file)
{
    std::vector<double> hits;
    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return hits;

    const auto frames = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (longestSeconds * reader->sampleRate));
    juce::AudioBuffer<float> audio ((int) reader->numChannels, frames);

    if (! reader->read (&audio, 0, frames, 0, true, true))
        return hits;

    for (const auto& onset : devices::detectOnsets (audio, reader->sampleRate))
        hits.push_back ((double) onset.sample / reader->sampleRate);

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
        }

        if (next == juce::File())
        {
            wake.wait (1000);
            continue;
        }

        auto hits = analyse (next);
        const std::scoped_lock scoped (lock);
        found[getKey (next)] = std::move (hits);
    }
}
} // namespace phazite
