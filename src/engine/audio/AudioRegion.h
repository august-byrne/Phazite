// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip as Phazite's engine plays it: a part of an audio file (AudioFile.h), from a sample of the song for so
// many, at a gain, faded in and out by one of Phazite's fade shapes, cut off at its edges. A file at another rate than
// the engine's plays through r8brain-free-src's resampling (Resampler.h). Built by the model from the song, with the file
// Phazite's renders of stretched, pitched and reversed clips are in when a clip needs one.
#pragma once

#include "AudioFile.h"

#include <memory>
#include <span>

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
// (after which it fades in over 40 samples rather than click, as it does once its file is read where it wasn't yet),
// where it is in its file, and for a file at another rate
// than the engine's, its resampling, from the file's sample it reads next.
struct RegionState
{
    // For a file at the engine's rate.
    RegionState();

    // For a file played by an engine at a rate: through a resampler for each of its channels (two at most) where their
    // rates differ, made here. Allocates: off the audio thread.
    RegionState (const AudioFile&, double engineRate);

    ~RegionState();

    // Whether it was made for a file at the rate and with the channels this one has, played at that rate.
    bool playsAt (const AudioFile&, double engineRate) const noexcept;

    std::int64_t playedTo = -1; // the song's sample after the last it played; -1 before it first plays
    double filePosition = 0.0;  // the file's sample it plays next (at another rate: it reads next)
    int fadingIn = 0;           // samples left of its fade in after a jump
    bool missed = false;        // the last stretch it played wasn't read yet: silence, and the next that is fades in

    // The region it last played as (its file, its start in the song and in the file): another (its clip moved, slipped or
    // rendered again under the playhead) plays from where the new one is there, as after a jump. Compared, never read.
    const AudioFile* playedFile = nullptr;
    std::int64_t playedStart = 0;
    double playedFileStart = 0.0;

    // A file at another rate's resampling, or nullptr; and a second, made ready ahead of where the region plays from next
    // without playing on to there (AudioRegion::prepare()), which then takes the first's place, so that it plays from there
    // at once rather than work its filter's start through in one block (known bug 85: every resampled clip did at the
    // loop's wrap, in the same block).
    struct Conversion;
    std::unique_ptr<Conversion> conversion, spare;
};

// A cut the transport makes in what plays (a stop, the loop's end): the sound fades out over so many samples before a
// sample of the song, and is silent from it on, rather than end in a step; none while its length is 0. A region that
// ends at that sample, or one sample after it (a stretched loop that rounds past the loop's end, which plays on whole
// into its start again), isn't cut, and plays as it would. A stop's cut plays on only the regions that were playing
// up to where it starts (continuesOnly): none starts in its fade.
struct RegionCut
{
    std::int64_t at = 0;
    int length = 0;
    bool continuesOnly = false;
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

    using Cut = RegionCut;

    // Audio thread, or a render's (which reads what it needs first): adds what the region plays from a sample of the song,
    // for so many samples, into two channels (a mono file into both), cut where the transport cuts it. Scratch holds what
    // it plays before its gain and fades: at least as many samples as are played, on two channels. A file at another rate
    // than the engine's plays only through a state made for the two (RegionState's), and otherwise as silence.
    void play (float* const* channels, std::int64_t songSample, int numSamples, double engineRate, bool isRender,
               juce::AudioBuffer<float>& scratch, Cut = {}) const noexcept;

    // Audio thread: a sample of the song it will play from next if it plays there, though the song doesn't play there now
    // (where playback will start, the loop's start): its file reads ahead of there too (AudioFile::expect()).
    void expect (std::int64_t songSample, double engineRate) const noexcept;

    // Audio thread, once a block: a file at another rate's resampling made ready, a little a block, for where it will play
    // from next without playing on to there: its start, half a second or less ahead of the sample the song plays next
    // (while it plays); else the first of the places given that it plays at (where playback will start, the loop's start).
    // What it plays from there then is the same, to the last bit, as from nothing.
    void prepare (std::int64_t songSample, bool playing, std::span<const std::int64_t> places, double engineRate,
                  int numSamples) const noexcept;

    // Audio thread: whether it is a file at another rate that plays at a sample of the song, read there, whose resampling
    // isn't made ready for there yet (prepare()): what a jump that lands there waits for (Engine.h: Landing). One whose file
    // isn't read there yet doesn't count: it plays as silence and fades in once it is, never waited for.
    bool needsPreparing (std::int64_t songSample, double engineRate) const noexcept;

    // Message thread: a sample of the song it plays from at once if it plays there (where a jump lands), which its file
    // reads first (AudioFile::want()).
    void want (std::int64_t songSample, double engineRate) const;
};
} // namespace phazite::engine
