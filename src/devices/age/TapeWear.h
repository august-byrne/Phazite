// SPDX-License-Identifier: AGPL-3.0-or-later
// A tape's wear, for Age's tape (Age.h): its hiss, and its dropouts.
//
// - The hiss is a tape's, brighter than a record's hush, and its two tracks share nothing: white noise from fixed seeds, each
//   side its own (dsp::Noise, as loud for each hertz at every rate), high-passed at 200 Hz (12 dB an octave) and lifted by a
//   high shelf of 7 dB at 5 kHz. Its level is its RMS at 48 kHz, measured after the filters, the same in each hertz at
//   every rate (0.37 dB less in all at 44.1 kHz, as Phazite's presets are leveled). It is multiplied by a share that glides between 0 and 1 over 20 ms (whether the tape plays,
//   as SurfaceNoise's does), and its level glides too.
// - The dropouts are the moments where the tape loses contact with the head: the sound's level dips, as a raised cosine down
//   and up, both sides together, and while it is down by d dB a low-pass of 12 dB an octave closes from 20 kHz toward 3 kHz
//   in proportion to d / 18 (on a log scale), so that the top goes first. One amount: at 0 none; from one every 8 seconds,
//   3 dB deep at most, at 10 % to two a second, 18 dB deep at most, at 100 % (the rate on a log scale, the depth straight between; under
//   10 % the depth falls to nothing at 0 and the rate goes on down its log scale). Each lasts 30 to 250 ms and dips from
//   half as deep to as deep as the amount says, both drawn for each.
// - The dropouts are where the song is, as the record's crackle is (SurfaceNoise.h): whether a step of the song's time (a
//   256th of a beat) starts one, how long it lasts and how deep it dips, come from a hash of the step, and the dip at a moment is the deepest
//   of those that started in the last 250 ms and haven't ended, so that a passage drops out the same however it is reached
//   (after a jump, the steps of the last 250 ms are drawn again). Stopped, the song doesn't move, and the dips fade away
//   over 20 ms. Real-time safe.
#pragma once

#include "devices/core/LiveValue.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Noise.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace phazite::devices
{
class TapeWear
{
public:
    static constexpr int stepsPerBeat = 256; // the grid of the song's time that dropouts are drawn on
    static constexpr int mostDips = 8;       // under way at once: one that comes while all are takes the oldest's place
    static constexpr double shortestDip = 0.03, longestDip = 0.25;                       // seconds
    static constexpr float deepestDip = 18.0f;                                           // decibels, at 100 %
    static constexpr double hissLowCut = 200.0, hissShelf = 5000.0, hissShelfGain = 7.0; // hertz, hertz and decibels

    // How many dropouts a second come at an amount (0 to 1), and how deep they dip, in decibels: none at 0.
    static double getDipsPerSecond (float amount) noexcept;
    static float getDipDepth (float amount) noexcept;

    void prepare (double sampleRate);
    void reset() noexcept;

    // As SurfaceNoise's: where the song is at the start of a stretch, in beats, how far it moves in a sample (0 while it is
    // stopped), and beats added to where it is.
    void setPosition (double beat, double beatsPerSample, double offset = 0.0) noexcept;

    // Moves on as far as some samples would, without playing them (while Age rests).
    void skip (int numSamples) noexcept { position += beatsPerSample * numSamples; }

    // The levels for what follows: the hiss's RMS as a gain (0 for none), and the dropouts' amount (0 to 1).
    void setLevels (float newHissGain, float newDropouts) noexcept
    {
        hissGain = newHissGain;
        dropouts = newDropouts;
    }

    // Starts the shares and the levels where they are going, without gliding there (the first block's).
    void jumpTo (bool hissWanted, bool dipsWanted) noexcept;

    // Whether anything sounds now: the hiss, or a dip under way (a dip of silence is silence, so dips to come don't count).
    bool isSounding() const noexcept { return hissShare > 0.0f || lastDip > 0.0f; }

    // For a display: how far the sound is dipped now, as a part of the deepest dip (0 to 1, set once a block by its device),
    // and how many dropouts came since it last looked.
    LiveValue& getDip() noexcept { return dipNow; }
    int takeDropouts() noexcept { return dipsMade.exchange (0, std::memory_order_relaxed); }

    // One sample: the sound dipped where a dropout is (while dips are wanted: the tape playing), then the hiss added (while
    // it is wanted). For a mono sound, pass the one channel twice and take the left.
    void process (float& left, float& right, bool hissWanted, bool dipsWanted) noexcept;

    // How far the sound is dipped at the last sample, in decibels (0 for none): for tests.
    float getLastDip() const noexcept { return lastDip; }

private:
    struct Dip
    {
        bool on = false;
        double start = 0.0, length = 1.0; // in beats
        float depth = 1.0f;               // as a part of the amount's depth: from a half to all of it, drawn for each
    };

    double sampleRate = 44100.0;
    double position = 0.0, beatsPerSample = 0.0, givenBeat = 0.0;
    bool hasPosition = false;
    std::int64_t lastStep = 0;
    bool hasStep = false;

    float hissGain = 0.0f, dropouts = 0.0f;
    float fadeStep = 0.001f, smoothing = 0.001f;
    float hissShare = 0.0f, hissNow = 0.0f, hissScale = 1.0f;
    dsp::FilterCoefficients hissLow, hissTop;
    std::array<dsp::Filter, 2> hissLowFilters, hissTopFilters;
    std::array<dsp::Noise, 2> hissNoise; // as loud for each hertz at every rate

    float dipShare = 0.0f, depthNow = 0.0f, lastDip = 0.0f;
    std::array<Dip, mostDips> dips {};
    std::array<dsp::Filter, 2> dipFilters;
    bool dipFiltering = false;

    LiveValue dipNow { 0.0f };
    std::atomic<int> dipsMade { 0 };
    static_assert (std::atomic<int>::is_always_lock_free);

    void drawSteps() noexcept;
    void drawStep (std::int64_t step, bool counts) noexcept;

    // The deepest dip now: how deep, as a part of the amount's depth, and how far through its raised cosine (0 to 1).
    struct Shape
    {
        float depth = 0.0f, along = 0.0f;
    };

    Shape getShape() const noexcept;
};
} // namespace phazite::devices
