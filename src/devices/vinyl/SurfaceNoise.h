// SPDX-License-Identifier: AGPL-3.0-or-later
// A record's surface noise, for the vinyl effect (Vinyl.h): hiss, the groove's steady hush, and crackle, the dust's clicks and
// pops at a density.
//
// - The hiss is the groove's hush as the playback curve leaves it: pink from 60 Hz to 2 kHz (as loud in each octave: alternate
//   poles and zeros an octave and a half apart, 3 dB an octave on average), and falling 6 dB an octave more from the RIAA
//   curve's 2.1 kHz (75 us), half alike on the two sides, as a record's two groove walls hiss. Its level is its RMS.
// - The crackle is clicks at random times, so many a second on average, most of them small and a few large, as dust is: each
//   one's size the fourth power of an even chance, so that half of them are 24 dB under the largest and one in nine within
//   4 dB of it. A click is the stylus thrown over a speck and back, as the cartridge plays it, the slope of a bump (a
//   Gaussian's derivative: up, then down): the smallest 15 us wide and bright, a tick; the largest eight times as wide and
//   dull, a pop. Each leans a little to one side.
// - The crackle is where the song is, not where playback began: whether a moment of the song crackles, and how, comes from its
//   place, in steps of a 4096th of a beat, each step's chance and its click drawn from a hash of the step (as a counter-based
//   generator draws them), so that a passage crackles the same way however it is reached, as the dust on a record would, and
//   playback, a render and a freeze play the same crackles. Stopped, the song doesn't move, and nothing crackles. The hiss
//   comes from seeds, the same after each reset.
//
// All of it is multiplied by a share that glides between 0 and 1 (the vinyl's: whether the record plays), and the hiss's
// level glides too, so that nothing clicks as they move. Real-time safe.
#pragma once

#include "devices/core/MeterValue.h"
#include "devices/dsp/Random.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace phazite::devices
{
class SurfaceNoise
{
public:
    static constexpr int stepsPerBeat = 4096; // the grid of the song's time that crackles are drawn on
    static constexpr int mostClicks = 8;      // sounding at once: one that comes while all of them sound takes the oldest's place
    static constexpr double hissTop = 2122.0; // where the hiss falls faster, in hertz: the RIAA curve's 75 us

    void prepare (double sampleRate);
    void reset() noexcept;

    // At the start of a stretch: where the song is, in beats, and how far it moves in a sample (0 while it is stopped). A
    // stretch given the same beat as the last (the song stopped) goes on from where the last one ended, and one given
    // another goes there: the next stretch's start, a loop going round, the playhead moved.
    void setPosition (double beat, double beatsPerSample, double offset = 0.0) noexcept; // offset: beats added to where it is

    // Moves on as far as some samples would, without playing them (while the vinyl rests).
    void skip (int numSamples) noexcept { position += beatsPerSample * numSamples; }

    // The levels for what follows: the hiss's RMS and the largest click's peak, as gains (0 for none), and how many clicks
    // come a second, on average.
    void setLevels (float newHissGain, float newCrackleGain, float newDensity) noexcept
    {
        hissGain = newHissGain;
        crackleGain = newCrackleGain;
        density = newDensity;
    }

    // Starts the share and the hiss's level where they are going, without gliding there (the first block's).
    void jumpTo (bool wanted) noexcept
    {
        share = wanted ? 1.0f : 0.0f;
        hissNow = hissGain;
    }

    // For a display: how many clicks came since it last looked, and the largest of them, as a part of the crackle's level.
    int takeClicks() noexcept { return clicksMade.exchange (0, std::memory_order_relaxed); }
    MeterValue& getLargest() noexcept { return largest; }

    // Whether anything sounds now: the share above nothing, or a click still ringing.
    bool isSounding() const noexcept { return share > 0.0f || activeClicks > 0; }

    // One sample's noise, added to both channels (for a mono sound, pass the one channel twice and take the left), the share
    // gliding toward 1 while it is wanted (the record playing) and toward 0 while it isn't.
    void addSample (float& left, float& right, bool wanted) noexcept
    {
        share = wanted ? std::min (1.0f, share + fadeStep) : std::max (0.0f, share - fadeStep);

        if (share > 0.0f)
        {
            if (beatsPerSample > 0.0 && crackleGain > 0.0f)
                drawSteps();

            hissNow += (hissGain - hissNow) * hissSmoothing;

            if (hissGain <= 0.0f && hissNow < 1.0e-9f)
                hissNow = 0.0f;

            if (hissNow > 0.0f)
            {
                const auto shared = commonNoise.nextBipolar();
                const auto gain = share * hissNow * hissScale;
                left += gain * hiss[0].process ((shared + ownNoise[0].nextBipolar()) * 0.70710678f, hissShape);
                right += gain * hiss[1].process ((shared + ownNoise[1].nextBipolar()) * 0.70710678f, hissShape);
            }
        }

        position += beatsPerSample;

        if (activeClicks > 0)
            playClicks (left, right);
    }

private:
    // The hiss's colour: two pole and zero pairs, then a pole, each first-order and matched (z = e^(s T)).
    struct HissShape
    {
        float poles[3] {}, zeros[2] {}, topGain = 1.0f;
    };

    // A channel's hiss: white noise through the shape.
    struct Hiss
    {
        float inputs[2] {}, outputs[3] {};

        float process (float white, const HissShape& shape) noexcept
        {
            const auto first = white - shape.zeros[0] * inputs[0] + shape.poles[0] * outputs[0];
            inputs[0] = white;
            outputs[0] = first;
            const auto second = first - shape.zeros[1] * inputs[1] + shape.poles[1] * outputs[1];
            inputs[1] = first;
            outputs[1] = second;
            outputs[2] = shape.topGain * second + shape.poles[2] * outputs[2];
            return outputs[2];
        }
    };

    struct Click
    {
        bool on = false;
        float at = 0.0f;                 // samples since it began
        float width = 1.0f;              // its Gaussian's sigma, in samples
        float length = 8.0f;             // eight of them, centred
        float left = 0.0f, right = 0.0f; // its peak on each side, signed
    };

    double sampleRate = 44100.0;
    double position = 0.0, beatsPerSample = 0.0, givenBeat = 0.0;
    bool hasPosition = false;
    std::int64_t lastStep = 0;
    bool hasStep = false;

    float hissGain = 0.0f, crackleGain = 0.0f, density = 0.0f;
    float share = 0.0f, fadeStep = 0.001f;
    float hissNow = 0.0f, hissSmoothing = 0.001f, hissScale = 1.0f;
    HissShape hissShape;
    std::array<Hiss, 2> hiss {};
    dsp::Random commonNoise;
    std::array<dsp::Random, 2> ownNoise;

    std::array<Click, mostClicks> clicks {};
    int activeClicks = 0;
    std::atomic<int> clicksMade { 0 };
    MeterValue largest { 0.0f };

    static_assert (std::atomic<int>::is_always_lock_free);

    void drawSteps() noexcept;
    void drawStep (std::int64_t step) noexcept;
    void playClicks (float& left, float& right) noexcept;
};
} // namespace phazite::devices
