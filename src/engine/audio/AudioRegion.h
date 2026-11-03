// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip as Phazite's engine plays it (docs/tracktion-migration.md, T2): a part of an audio file (AudioFile.h), from a
// sample of the song for so many, at a gain, faded in and out by one of Phazite's fade shapes, cut off at its edges. A file
// at another rate than the engine's plays through 5-point Lagrange interpolation, as Tracktion played it. Built by the model
// from the song, with the file Phazite's renders of stretched, pitched and reversed clips are in when a clip needs one.
#pragma once

#include "AudioFile.h"

#include <array>
#include <memory>

namespace phazite::engine
{
// A fade's shape, as Phazite's audio clips have them: a curve or an S-curve, bent from -1 to 1. A curve bent to 1 is convex
// (quick at first: a quarter of a sine), to -1 concave (slow at first); an S-curve bent to 1 starts and ends slowly, to -1
// quickly. Either is straight at 0, and between, it is that much of the way from straight to its shape at 1 or -1.
enum class FadeShape
{
    curve,
    sCurve
};

struct FadeCurve
{
    FadeShape shape = FadeShape::curve;
    float bend = 0.0f;
};

// A fade in's gain, along it from 0 (its start) to 1; a fade out's is the same at 1 - along.
float getFadeGain (FadeCurve, float along) noexcept;

// What a region keeps from one block to the next, by its clip's ID from plan to plan: where it played to, to tell a jump
// (after which it fades in over 40 samples rather than click), and where it is in a file at another rate.
struct RegionState
{
    std::int64_t playedTo = -1; // the song's sample after the last it played; -1 before it first plays
    double filePosition = 0.0;
    std::array<juce::LagrangeInterpolator, 2> interpolators;
    int fadingIn = 0; // samples left of its fade in after a jump
};

struct AudioRegion
{
    std::shared_ptr<AudioFile> file;
    std::shared_ptr<RegionState> state;
    std::int64_t start = 0, length = 0; // where it plays in the song, in samples at the engine's rate
    double fileStart = 0.0;             // the file's sample, at its own rate, that plays at the region's start
    float gain = 1.0f;
    int fadeIn = 0, fadeOut = 0; // in samples, fitted to the region
    FadeCurve fadeInCurve, fadeOutCurve;

    std::int64_t getEnd() const noexcept { return start + length; }

    // Audio thread, or a render's (which reads what it needs first): adds what the region plays from a sample of the song,
    // for so many samples, into two channels (a mono file into both). Scratch holds what a file at another rate is read into:
    // at least 8 times as many samples as are played, on two channels.
    void play (float* const* channels, std::int64_t songSample, int numSamples, double engineRate, bool isRender,
               juce::AudioBuffer<float>& scratch) const noexcept;
};
} // namespace phazite::engine
