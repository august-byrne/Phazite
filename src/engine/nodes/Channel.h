// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's own controls as Phazite's engine plays them (docs/tracktion-migration.md, T2): its fader's volume and pan,
// whether it is heard (its mute, and the solos, which the model works out), and its meter. The message thread sets each,
// one at a time, without building anything; the audio thread eases to them, or follows the volume's and pan's automation.
// Kept by the track's ID from one plan to the next.
#pragma once

#include "LevelMeter.h"
#include "engine/automation/Curve.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <span>

namespace phazite::engine
{
// How a pan of -1 (left) to 1 (right) turns each side up or down: constant power, with the sound at its level in the
// middle (docs/design.md, section 8): a hard pan plays its side 3 dB louder and the other not at all, and a sound keeps
// its loudness wherever it is panned. On a stereo channel, each side is turned so, as a balance.
struct PanGains
{
    float left = 1.0f, right = 1.0f;
};

PanGains getPanGains (float pan) noexcept;

// The fader's travel, as automation draws it and modulation moves it: a position from 0 to 1 is 20 ln (position) + 6
// decibels, 0 dB at about 0.74 and +6 dB at the top, as Tracktion's faders go (Tracktion's volumeFaderPositionToDB()); 0 is
// silence. And back.
float faderPositionToDecibels (float position) noexcept;
float decibelsToFaderPosition (float decibels) noexcept;

class Channel
{
public:
    static constexpr float silenceDb = -100.0f; // and below: no sound

    // Message thread.
    void setVolume (float decibels) noexcept { volume.store (decibels, std::memory_order_relaxed); }
    void setPan (float newPan) noexcept { pan.store (newPan, std::memory_order_relaxed); }
    void setAudible (bool isHeard) noexcept { audible.store (isHeard, std::memory_order_relaxed); }
    float getVolume() const noexcept { return volume.load (std::memory_order_relaxed); }
    float getPan() const noexcept { return pan.load (std::memory_order_relaxed); }
    bool isAudible() const noexcept { return audible.load (std::memory_order_relaxed); }

    // Before playback (it may allocate nothing, but must be told the rate).
    void prepare (double sampleRate) noexcept;

    // Audio thread: whether the channel plays nothing now, muted (or soloed out) and faded out, so that what feeds it
    // needn't play.
    bool isSilenced() const noexcept { return started && fading == 0 && juce::exactlyEqual (heard, 0.0f) && ! isAudible(); }

    // Message thread: whether the volume or the pan is written by hand now (automation's touch, latch and write modes), so
    // that it plays its value set by hand rather than its automation.
    void setVolumeWriting (bool isWriting) noexcept { volumeWriting.store (isWriting, std::memory_order_relaxed); }
    void setPanWriting (bool isWriting) noexcept { panWriting.store (isWriting, std::memory_order_relaxed); }

    // Any thread: the volume, in decibels, and pan it played last, its automation's or the values set by hand, with its
    // modulation or without.
    float getPlayedVolume (bool withModulation = true) const noexcept
    {
        return (withModulation ? playedVolume : playedBaseVolume).load (std::memory_order_relaxed);
    }
    float getPlayedPan (bool withModulation = true) const noexcept
    {
        return (withModulation ? playedPan : playedBasePan).load (std::memory_order_relaxed);
    }

    // A block's automation of the volume (in the fader's positions) and the pan, either of them none, and where the song
    // is: while it plays, the fader follows them exactly, at their values on the song's control points and in a straight
    // line between; stopped, it eases to their values where the song is, as to a value set by hand. And the block's
    // modulation of each, as parts of its range on each control point from the one at or before the block's start (none
    // for none), which moves it from there, in a straight line between the points.
    struct Automation
    {
        const Curve* volume = nullptr;
        const Curve* pan = nullptr;
        double beat = 0.0;
        double beatsPerSample = 0.0; // 0 while the song is stopped
        std::int64_t sample = 0;     // the song's sample at the block's start
        std::span<const float> volumeModulation, panModulation;
    };

    // Audio thread: turns two channels (or one, the same on both sides) by the fader, easing to a new volume or pan set
    // by hand over 15 ms, following its automation, and fading in or out over 5 ms as the channel is heard or not; then
    // measures what comes out.
    void process (float* const* channels, int numChannels, int numSamples, const Automation& = {}) noexcept;

    LevelMeter& getMeter() noexcept { return meter; }

private:
    std::atomic<float> volume { 0.0f }, pan { 0.0f }, playedVolume { 0.0f }, playedPan { 0.0f }, playedBaseVolume { 0.0f },
        playedBasePan { 0.0f };
    std::atomic<bool> audible { true }, volumeWriting { false }, panWriting { false };

    // The audio thread's: each side's gain now and where it eases to, how many samples are left to get there and how much it
    // moves in each; and the mute's fade, likewise.
    double sampleRate = 44100.0;
    float gains[2] = { 1.0f, 1.0f }, steps[2] = {};
    float targetVolume = 0.0f, targetPan = 0.0f, baseVolume = 0.0f, basePan = 0.0f;
    int easing = 0;
    bool started = false;
    float heard = 1.0f, heardTarget = 1.0f, heardStep = 0.0f;
    int fading = 0;

    LevelMeter meter;
};
} // namespace phazite::engine
