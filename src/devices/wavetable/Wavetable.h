// SPDX-License-Identifier: AGPL-3.0-or-later
// A wavetable (roadmap step 2.13): single-cycle waves, its frames, that an oscillator morphs through, as Serum's, Vital's
// and Ableton Live's Wavetable do. Each frame is kept band-limited at every pitch: for each half octave, a copy with only
// the harmonics whose aliases stay above hearing, made with an FFT when the table is made, which the oscillator reads
// with cubic interpolation, crossfading between neighbouring frames. So a plain table plays without aliasing, as
// Ableton's manual promises of its own. A table is made off the audio thread, from its frames' harmonics or samples, and
// never changes once made.
#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <complex>
#include <memory>
#include <span>
#include <vector>

namespace phazite::devices
{
class Wavetable
{
public:
    static constexpr int frameSize = 2048; // samples in a frame read from a sound, as Serum's tables have
    static constexpr int maxHarmonics = frameSize / 2;
    static constexpr int maxFrames = 256;
    static constexpr int numLevels = 21; // from every harmonic down to the first alone, a level each half octave

    // A frame's harmonics: element h - 1 is harmonic h, as an amplitude and a phase: the frame is the sum, over them,
    // of Re (c e^(i 2 pi h t)) for t from 0 to 1 across its cycle. Up to maxHarmonics of them.
    using Harmonics = std::vector<std::complex<float>>;

    // Off the audio thread: a table from its frames' harmonics, or from their samples (each frame its own length, a
    // whole cycle), with any constant offset taken out and the loudest frame's peak at 1. Of at least one frame (a
    // silent one if none is given) and at most maxFrames.
    static std::shared_ptr<const Wavetable> fromHarmonics (const juce::String& name, const std::vector<Harmonics>& frames);
    static std::shared_ptr<const Wavetable> fromFrames (const juce::String& name, const std::vector<std::vector<float>>& frames);

    // A table from a sound, as Serum reads one: frames of samplesPerFrame when that is given (from a WAV file's 'clm '
    // chunk); otherwise frames of frameSize when the sound is a whole number of them, the whole sound as one frame when
    // it is shorter than two (a single cycle), and else its first frames of frameSize. At most maxFrames.
    static std::shared_ptr<const Wavetable> fromSound (const juce::String& name, std::span<const float> samples, int samplesPerFrame = 0);

    const juce::String& getName() const noexcept { return name; }
    int getNumFrames() const noexcept { return numFrames; }

    // A frame's wave with all its harmonics, for drawing: the samples of one cycle.
    std::span<const float> getFrameForDrawing (int frame) const noexcept;

    // A frame's wave at a level, for the oscillator: `length` samples over one cycle (a power of two), with the last one
    // before the first and the first two after the last, for interpolation. Real-time safe.
    struct Level
    {
        const float* samples = nullptr; // the cycle's first sample
        int length = 0;
    };

    Level getLevel (int frame, int level) const noexcept;

    // How many harmonics a level keeps: all of them at level 0, about 1/sqrt(2) as many at each level after.
    static int getHarmonics (int level) noexcept;

    // The level that keeps the most harmonics while its highest is at most `highestHarmonic`, the highest that may play
    // (one whose alias would be heard may not). Real-time safe.
    static int chooseLevel (double highestHarmonic) noexcept;

private:
    juce::String name;
    int numFrames = 0;
    std::vector<float> samples;                 // each frame's levels, one frame after another
    std::array<int, numLevels> levelOffsets {}; // where each level's first sample is within a frame's block
    std::array<int, numLevels> levelLengths {};
    int frameStride = 0; // the samples each frame takes

    Wavetable() = default;
};

// A level's wave at a place in its cycle (in samples, from 0 up to its length), with 4-point cubic (Catmull-Rom)
// interpolation. Real-time safe.
inline float readCubic (const Wavetable::Level& level, float place) noexcept
{
    const auto index = (int) place;
    const auto t = place - (float) index;
    const auto* s = level.samples + index;
    const auto a = s[-1], b = s[0], c = s[1], d = s[2];
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}
} // namespace phazite::devices
