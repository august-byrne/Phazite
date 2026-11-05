// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Comparison.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
namespace
{
    // Adds a channel into another at a gain going evenly from one value to another over the stretch.
    void addMoving (float* destination, const float* source, float from, float to, int numSamples) noexcept
    {
        if (juce::exactlyEqual (from, to))
        {
            if (! juce::exactlyEqual (to, 0.0f))
                juce::FloatVectorOperations::addWithMultiply (destination, source, to, numSamples);

            return;
        }

        const auto step = (to - from) / (float) numSamples;

        for (int i = 0; i < numSamples; ++i)
            destination[i] += source[i] * (from + step * (float) (i + 1));
    }
} // namespace

void ComparisonState::prepare (double rate, int blockSize)
{
    sampleRate = rate;
    maximumBlockSize = blockSize;
    songMeter.prepare (rate, blockSize);
    versionMeter.prepare (rate, blockSize);
    sound.setSize (2, blockSize);
    scratch.setSize (2, blockSize * 9 + 16); // a file at up to 8 times the engine's rate (AudioRegion.h)
}

void ComparisonState::hear (bool version) noexcept
{
    hearingVersion.store (version, std::memory_order_release);
    switchesAsked.fetch_add (1, std::memory_order_acq_rel);
}

void ComparisonState::match() noexcept
{
    const auto song = songMeter.getReadings().integrated, version = versionMeter.getReadings().integrated;

    if (! song.has_value() || ! version.has_value())
        return;

    // How much louder the version is (quieter, below 0), in LU: that much off the louder of the two.
    const auto louder = *version - *song;
    songGain.store (juce::Decibels::decibelsToGain (std::min (0.0f, louder)), std::memory_order_relaxed);
    versionGain.store (juce::Decibels::decibelsToGain (-std::max (0.0f, louder)), std::memory_order_relaxed);
    matchWanted = false;
}

void ComparisonState::process (const AudioRegion& bounce, const TempoMap& songTempo, const TempoMap& versionTempo, const float* const* song,
                               float* const* outputs, int numOutputs, std::int64_t songSample, int numSamples, bool playing) noexcept
{
    numOutputs = std::min (numOutputs, 2);

    // Longer than it was prepared for (it never is: the engine plays its blocks in parts of the prepared size): the song.
    if (numSamples > maximumBlockSize)
    {
        for (int channel = 0; channel < numOutputs; ++channel)
            if (outputs[channel] != nullptr)
                juce::FloatVectorOperations::add (outputs[channel], song[channel], numSamples);

        return;
    }

    // A switch: the gains matched again, and the bounce found again at the song's beat.
    if (const auto asked = switchesAsked.load (std::memory_order_acquire); asked != switchesTaken)
    {
        switchesTaken = asked;
        matchWanted = true;
        nextSongSample = -1;
    }

    if (matchWanted)
        match();

    const auto version = hearingVersion.load (std::memory_order_acquire);
    const auto toSong = ! playing ? 1.0f : version ? 0.0f : songGain.load (std::memory_order_relaxed);
    const auto toVersion = playing && version ? versionGain.load (std::memory_order_relaxed) : 0.0f;

    if (playing)
    {
        // Where the bounce is: the song's beat in the version's time after a jump, a start or a switch; on from there.
        if (songSample != nextSongSample)
            position = std::llround (versionTempo.toSamples (songTempo.toBeats ((double) songSample)));

        nextSongSample = songSample + numSamples;
        sound.clear (0, numSamples);
        float* channels[] = { sound.getWritePointer (0), sound.getWritePointer (1) };
        bounce.play (channels, position, numSamples, sampleRate, false, scratch);
        position += numSamples;

        // Both measured as they are, before their gains.
        const float* versionSound[] = { sound.getReadPointer (0), sound.getReadPointer (1) };
        songMeter.process (song, 2, numSamples);
        versionMeter.process (versionSound, 2, numSamples);
    }
    else
    {
        nextSongSample = -1; // playback starting finds the bounce again
    }

    for (int channel = 0; channel < numOutputs; ++channel)
    {
        if (outputs[channel] == nullptr)
            continue;

        addMoving (outputs[channel], song[channel], playedSongGain, toSong, numSamples);

        if (playing)
            addMoving (outputs[channel], sound.getReadPointer (channel), playedVersionGain, toVersion, numSamples);
    }

    playedSongGain = toSong;
    playedVersionGain = toVersion;
}
} // namespace phazite::engine
