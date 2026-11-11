// SPDX-License-Identifier: AGPL-3.0-or-later
// Safe Bass, the phaser's, the flanger's and the chorus's: what is below a frequency goes around the effect and back in
// after it, as it was. The sound is split there by a Linkwitz-Riley crossover (dsp/Crossover.h, as the Multiband's bands
// and the stereo expander's Bass Mono are): the effect hears only the band above it, which is 6 dB down at the frequency
// and falls 24 dB an octave below it, as the band below falls above it. The two bands add up to the sound through an
// all-pass, as loud as it was at every frequency; so the sound its runner mixes back in (its mix: ProcessContext::dry) is
// turned the same, the two bands added up, and stays in phase with them at any mix, as the Multiband turns the sound it
// mixes back in. At its lowest, 20 Hz, it is off: the effect hears the sound untouched and the runner mixes it back in as it
// came. Turned on, it fades in over 10 ms at 5 Hz, under what is heard, then glides up to its frequency in octaves; turned
// off, it glides down there and fades out; and it glides from one frequency to another however far its value jumps, so
// that none of these clicks. Real-time safe.
#pragma once

#include "devices/dsp/Crossover.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace phazite::devices
{
class SafeBass
{
public:
    static constexpr float lowest = 20.0f; // its lowest: off

    static bool isOn (float hertz) noexcept { return hertz > lowest + 0.5f; }

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        fadePerSample = (float) (1.0 / (fadeSeconds * sampleRate));
        octavesPerSample = (float) (octavesPerSecond / sampleRate);
        reset();
    }

    // Silence in its filters, and where its value puts it at once from the next block.
    void reset() noexcept
    {
        for (auto& crossover : splits)
            crossover.reset();

        amount = from = step = 0.0f;
        whole = false;
        fresh = true;
    }

    // As a block starts: Safe Bass's value.
    void startBlock (float hertz) noexcept
    {
        on = isOn (hertz);
        target = on ? hertz : fadeFrequency;

        if (std::exchange (fresh, false))
        {
            amount = on ? 1.0f : 0.0f;
            setFrequency (target);
        }
    }

    // As a stretch of a block starts (an effect's control step), so many samples long: the fade and the glide move on over
    // it. Whether it splits the sound in the stretch at all: not while it is off and faded out.
    bool startStretch (int length) noexcept
    {
        const auto samples = (float) length;
        from = amount;

        if (on && amount < 1.0f)
        {
            // Fading in where it fades, from silence if it was off.
            if (amount <= 0.0f)
            {
                for (auto& crossover : splits)
                    crossover.reset();

                setFrequency (fadeFrequency);
            }

            amount = std::min (1.0f, amount + fadePerSample * samples);
        }
        else if (! juce::exactlyEqual (frequency, target))
        {
            // In octaves, toward its value, or down to where it fades out.
            const auto octaves = std::log2 (target / frequency), most = octavesPerSample * samples;
            setFrequency (std::abs (octaves) <= most ? target : frequency * std::exp2 (octaves > 0.0f ? most : -most));
        }
        else if (! on && amount > 0.0f)
        {
            amount = std::max (0.0f, amount - fadePerSample * samples);
        }

        step = (amount - from) / samples;
        whole = from >= 1.0f && amount >= 1.0f;
        return from > 0.0f || amount > 0.0f;
    }

    // A side's sample, the i-th of the stretch: the band the effect hears, the band that goes around it, and the sound for
    // its runner to mix back in. While it fades, each is between the sound as it came and the split.
    struct Parts
    {
        float through, around, dry;
    };

    Parts split (int channel, float x, int i) noexcept
    {
        const auto bands = splits[(size_t) channel].process (x);
        const auto both = bands.low + bands.high;

        if (whole)
            return { bands.high, bands.low, both };

        const auto faded = from + step * (float) (i + 1);
        return { x + faded * (bands.high - x), faded * bands.low, x + faded * (both - x) };
    }

private:
    static constexpr double fadeSeconds = 0.01;
    static constexpr double octavesPerSecond = 300.0; // its glide's speed: two octaves in under 7 ms
    static constexpr float fadeFrequency = 5.0f;      // where it fades in and out: under what is heard

    double sampleRate = 48000.0;
    float fadePerSample = 0.002f, octavesPerSample = 0.006f;
    bool on = false, fresh = true;
    float target = fadeFrequency, frequency = fadeFrequency; // where it glides, and where it is
    float amount = 0.0f, from = 0.0f, step = 0.0f;           // the fade: now, as the stretch started, and a sample's step
    bool whole = false;                                      // all the way in over the stretch
    std::array<dsp::Crossover, 2> splits;

    void setFrequency (float hertz) noexcept
    {
        frequency = hertz;
        splits[0].setFrequency (hertz, sampleRate);
        splits[1].setFrequencyAs (splits[0]);
    }
};
} // namespace phazite::devices
