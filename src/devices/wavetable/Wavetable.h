// SPDX-License-Identifier: AGPL-3.0-or-later
// A wavetable (roadmap step 2.13): single-cycle waves, its frames, that an oscillator morphs through, as Serum's, Vital's
// and Ableton Live's Wavetable do. Each frame is kept band-limited at every pitch: for each half octave, a copy with only
// the harmonics whose aliases stay above hearing, made with an FFT when the table is made, with 16 samples for each cycle of
// its highest harmonic (up to 8192 a copy), which the oscillator reads with cubic interpolation, crossfading between
// neighbouring frames. So a plain table plays without aliasing, as
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

    // A table for an oscillator that reads one frame (devices/dsp/Oscillators.h): its harmonics at the level they give,
    // and each level with 32 samples for each cycle of its highest harmonic rather than 16, so that the oscillator reads it
    // by straight lines between its samples, cheaply, and what that adds is more than 80 dB down.
    static std::shared_ptr<const Wavetable> forOscillator (const juce::String& name, const Harmonics& frame);
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

    // What a warp that reads only a part of a frame's cycle, or reads it unevenly, leaves off centre (the synth's
    // Sync and Bend): every frame is centred over its whole cycle, but not over a part of it. Worked out as the table is
    // made. Real-time safe.
    //
    // A frame's level's area from its cycle's start to a place in it (from 0 to 1): the integral of its wave, 0 over the
    // whole cycle. Kept at areaPoints places across the cycle, each worked out exactly from the harmonics the level keeps,
    // and read between them along a straight line.
    float getArea (int frame, int level, float place) const noexcept;

    // The mean of a frame's level read across its cycle with its places bent, place = t b / (1 + (b - 1) t) for t from 0 to
    // 1: its start squeezed and its end stretched for a bend above 1, the other way below. Worked out exactly from the
    // harmonics the level keeps, at bends from 1/8 to 8, bendsPerOctave to an octave, and read between them: where a bend
    // falls among them (findBend()), the same for every frame and level, found once for all of them.
    struct Bend
    {
        int index = 0;
        float fraction = 0.0f;
    };

    static Bend findBend (float bend) noexcept;
    float getBentMean (int frame, int level, Bend) const noexcept;

    static constexpr int areaPoints = 128;   // the places across a cycle each level's area is kept at, and its end
    static constexpr int bendsPerOctave = 8; // the bends each level's bent mean is kept at, from 1/8 to 8
    static constexpr int numBends = 6 * bendsPerOctave + 1;

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
    int frameStride = 0;          // the samples each frame takes
    std::vector<float> areas;     // each frame's levels' areas at areaPoints + 1 places, one after another
    std::vector<float> bentMeans; // each frame's levels' bent means at numBends bends, one after another

    Wavetable() = default;

    void findAreasAndMeans (const std::vector<Harmonics>& frames, float scale); // once its frames are made, scaled as they were

    static std::shared_ptr<const Wavetable> make (const juce::String& name, const std::vector<Harmonics>& frames, bool peakAtOne,
                                                  int samplesPerCycle, int longestLevel);
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
