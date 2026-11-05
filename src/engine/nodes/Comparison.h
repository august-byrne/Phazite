// SPDX-License-Identifier: AGPL-3.0-or-later
// A version's bounce heard against the song (the model's versions, docs/versions.md: Hearing a version), as Phazite's engine
// plays it after the master: what goes out is the song or the bounce, which both play while the comparison lasts, each
// measured as it is (EBU R128, LoudnessMeter.h), and at each switch the louder of the two is turned down to the other's
// loudness so far, so that neither wins by being louder (until both have a loudness, as soon as they do). The bounce plays
// from where the song's beat is in the version's own time (its tempo map), found again as playback starts, jumps or loops
// and at each switch, and goes on at its own pace in between. Each switch, and playback stopping or starting, moves the gains
// over a block. What goes out while the song is stopped is the song (its tails, live notes) at its own level.
//
// What a plan points at (Plan.h: Plan::Comparison), as it does at what sounds on from one plan to the next, kept by the model
// while the comparison lasts.
#pragma once

#include "LoudnessMeter.h"
#include "engine/audio/AudioRegion.h"
#include "engine/core/TempoMap.h"

#include <atomic>
#include <cstdint>

namespace phazite::engine
{
class ComparisonState
{
public:
    // Message thread, before a plan points at it (it allocates): the engine's rate and its blocks' longest.
    void prepare (double sampleRate, int maximumBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }
    int getMaximumBlockSize() const noexcept { return maximumBlockSize; }

    // Any thread: which is heard, the version or the song, from the audio thread's next block; each call is a switch.
    void hear (bool version) noexcept;
    bool isHearingVersion() const noexcept { return hearingVersion.load (std::memory_order_acquire); }

    // Any thread: how loud each has been, measured before its gain since the comparison began; and the gains the last
    // switch gave them, as gains (1 for none).
    LoudnessMeter::Readings getSongLoudness() const noexcept { return songMeter.getReadings(); }
    LoudnessMeter::Readings getVersionLoudness() const noexcept { return versionMeter.getReadings(); }
    float getSongGain() const noexcept { return songGain.load (std::memory_order_relaxed); }
    float getVersionGain() const noexcept { return versionGain.load (std::memory_order_relaxed); }

    // The audio thread: adds what goes out for a stretch of the song into the outputs (up to two): the song's sound (the
    // master's, two channels) at a sample of the song, by the song's tempo map, and the bounce (a region of the whole file
    // from the version's start) by the version's, while playing.
    void process (const AudioRegion& bounce, const TempoMap& songTempo, const TempoMap& versionTempo, const float* const* song,
                  float* const* outputs, int numOutputs, std::int64_t songSample, int numSamples, bool playing) noexcept;

private:
    double sampleRate = 48000.0;
    int maximumBlockSize = 0;
    LoudnessMeter songMeter, versionMeter;
    juce::AudioBuffer<float> sound, scratch; // the bounce's sound for a block; what its file is read into

    std::atomic<bool> hearingVersion { false };
    std::atomic<std::uint32_t> switchesAsked { 0 };
    std::atomic<float> songGain { 1.0f }, versionGain { 1.0f };

    // The audio thread's: the switches taken; whether the gains still wait for both loudnesses; the gains of the last block
    // played, which the next moves from; where the bounce is (in the version's samples) and where the song will be next if it
    // plays on without a jump.
    std::uint32_t switchesTaken = 0;
    bool matchWanted = false;
    float playedSongGain = 1.0f, playedVersionGain = 0.0f;
    std::int64_t position = 0, nextSongSample = -1;

    void match() noexcept; // the louder turned down to the other's loudness so far, if both have one
};
} // namespace phazite::engine
