// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's own controls as Phazite's engine plays them (docs/tracktion-migration.md, T2): its fader's volume and pan,
// whether it is heard (its mute, and the solos, which the model works out), and its meter. The message thread sets each,
// one at a time, without building anything; the audio thread eases to them. Kept by the track's ID from one plan to the
// next.
#pragma once

#include "LevelMeter.h"

#include <juce_core/juce_core.h>

#include <atomic>

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

    // Audio thread: turns two channels (or one, the same on both sides) by the fader, easing to a new volume or pan over
    // 15 ms and fading in or out over 5 ms as the channel is heard or not; then measures what comes out.
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    LevelMeter& getMeter() noexcept { return meter; }

private:
    std::atomic<float> volume { 0.0f }, pan { 0.0f };
    std::atomic<bool> audible { true };

    // The audio thread's: each side's gain now and where it eases to, how many samples are left to get there and how much it
    // moves in each; and the mute's fade, likewise.
    double sampleRate = 44100.0;
    float gains[2] = { 1.0f, 1.0f }, steps[2] = {};
    float targetVolume = 0.0f, targetPan = 0.0f;
    int easing = 0;
    bool started = false;
    float heard = 1.0f, heardTarget = 1.0f, heardStep = 0.0f;
    int fading = 0;

    LevelMeter meter;
};
} // namespace phazite::engine
