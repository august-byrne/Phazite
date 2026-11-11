// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FactorySamples.h"
#include "devices/drums/DrumSynthesis.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/SpectralLoop.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <vector>

namespace phazite::devices
{
namespace
{
    constexpr double rate = 48000.0;                  // what they are made at: the sampler plays them at any rate
    constexpr std::string_view bassName = "808 Bass"; // the first
    constexpr int bassRoot = 36;                      // C2, 65.4 Hz
    constexpr int plainRoot = 60;                     // middle C, for a sound without a clear pitch

    // An 808 played as a bass line: a long sine at C2 that drops onto its note from an octave above in a few tens of
    // milliseconds, with a click, driven into a gentle saturation that gives it the harmonics that carry it on small
    // speakers.
    juce::AudioBuffer<float> makeBass()
    {
        return drums::makeKick (rate, 130.81, 65.406, 0.03, 1.4, 3.0, 0.08, 1.8);
    }

    // The sounds made from their spectra (dsp/SpectralLoop.h), after the bass: each a tone's harmonics, spread into bands
    // and made into one seamless loop, the right side the same loop from half-way round, which is as good as unrelated to
    // the left, so the sound is as wide as two players are. Another is a line here: its name, the note it is made at, its
    // length, its harmonics' levels, how wide their bands are, and the seed of its phases.
    struct SpectralSound
    {
        std::string_view name;
        int root = 57;  // the note it is made at, which plays it at its own pitch
        int order = 18; // it is 2^order samples long: at 48 kHz, 5.5 s
        float (*level) (int harmonic, double hertz) = nullptr;
        double bandwidth = 20.0, bandwidthScale = 1.0; // dsp::SpectralLoop's
        std::uint32_t seed = 1;

        // Where in the loop the sound starts, which every note plays from while Start Random is off: of 1024 places evenly
        // round it, the one whose first 400 ms are most like the whole on both sides, each of its first eight harmonics near
        // its level, the two sides as loud as each other and as the whole (found once, by a port of this code, so that making
        // the sound doesn't search). From anywhere else a short note can come out several decibels off, or to one side.
        int start = 0;
    };

    // "Wide Saw": a saw's harmonics, each 1/n, in bands with a power standard deviation of 4.8 cents (a bandwidth of 19.14
    // as Nasca counts it), as wide in cents at every harmonic: what a section of players of one saw, each a few cents off
    // the others, sounds like, with none of the beating a few detuned oscillators have. After ZynAddSubFX's PADsynth at a
    // bandwidth of 18.4 cents with its Gaussian profile, whose bands measure 4.8 cents. Made at A3, so that it has every
    // harmonic up to 24 kHz down to there and half of them an octave below; its loop is 5.5 s there, and 2.7 s an octave
    // up, as long as ZynAddSubFX's own.
    constexpr SpectralSound spectralSounds[] = {
        { .name = "Wide Saw",
          .root = 57,
          .order = 18,
          .level = [] (int harmonic, double) { return 1.0f / (float) harmonic; },
          .bandwidth = 19.14,
          .bandwidthScale = 1.0,
          .seed = 0x5a77u,
          .start = 55296 },
    };

    constexpr int numSpectral = (int) std::size (spectralSounds);
    constexpr float spectralLevel = -18.0f; // their RMS, in decibels: their peaks are 12 to 14 dB over it

    void makeSpectral (Sampler::Sound& sound, const SpectralSound& recipe)
    {
        const auto fundamental = dsp::noteToFrequency (recipe.root);
        std::vector<float> harmonics;

        for (int number = 1; number * fundamental < 0.5 * rate; ++number)
            harmonics.push_back (recipe.level (number, number * fundamental));

        const auto loop = dsp::makeSpectralLoop ({ .harmonics = harmonics,
                                                   .fundamental = fundamental,
                                                   .bandwidth = recipe.bandwidth,
                                                   .bandwidthScale = recipe.bandwidthScale,
                                                   .sampleRate = rate,
                                                   .order = recipe.order,
                                                   .seed = recipe.seed });
        const auto length = (int) loop.size();
        const auto gain = dsp::decibelsToGain (spectralLevel);
        sound.audio.setSize (2, length);
        sound.sampleRate = rate;
        sound.seamless = true;

        for (int channel = 0; channel < 2; ++channel)
        {
            auto* samples = sound.audio.getWritePointer (channel);
            const auto from = recipe.start + channel * (length / 2);

            for (int i = 0; i < length; ++i)
                samples[i] = gain * loop[(size_t) ((from + i) % length)];
        }

        // A steady sound: nothing in it to slice at but its start.
        sound.onsets = { Onset {} };
    }

    const std::vector<std::string_view>& getNames()
    {
        static const std::vector<std::string_view> names = []
        {
            std::vector<std::string_view> made { bassName };

            for (const auto& each : spectralSounds)
                made.push_back (each.name);

            const auto drumNames = getFactoryDrumNames();
            made.insert (made.end(), drumNames.begin(), drumNames.end());
            return made;
        }();

        return names;
    }

    std::shared_ptr<const Sampler::Sound> make (size_t index)
    {
        auto sound = std::make_shared<Sampler::Sound>();

        if (index == 0)
        {
            sound->audio = makeBass();
            sound->sampleRate = rate;

            // A short fade at its end, so that held long it stops cleanly.
            const auto length = sound->audio.getNumSamples();
            sound->audio.applyGainRamp (length - length / 10, length / 10, 1.0f, 0.0f);
        }
        else if (index <= (size_t) numSpectral)
        {
            // At its own level, by its RMS: its peak is wherever its phases put it, which says little of how loud it is.
            makeSpectral (*sound, spectralSounds[index - 1]);
            Sampler::makeOctaves (*sound);
            return sound;
        }
        else
        {
            const auto drum = getFactoryDrum ((int) index - 1 - numSpectral);
            sound->audio = drum->audio;
            sound->sampleRate = drum->sampleRate;
        }

        if (const auto peak = sound->audio.getMagnitude (0, sound->audio.getNumSamples()); peak > 0.0f)
            sound->audio.applyGain (dsp::decibelsToGain (-6.0f) / peak);

        sound->onsets = detectOnsets (sound->audio, sound->sampleRate);
        Sampler::makeOctaves (*sound);
        return sound;
    }
} // namespace

std::span<const std::string_view> getFactorySampleNames()
{
    return getNames();
}

int getNumPitchedFactorySamples()
{
    return 1 + numSpectral;
}

int findFactorySample (std::string_view name)
{
    const auto& names = getNames();
    const auto found = std::find (names.begin(), names.end(), name);
    return found != names.end() ? (int) (found - names.begin()) : -1;
}

std::shared_ptr<const Sampler::Sound> getFactorySample (int index)
{
    const auto& names = getNames();
    const auto at = juce::isPositiveAndBelow (index, (int) names.size()) ? (size_t) index : 0;

    // Each one made once, while something holds it; the first for good, as every new sampler plays it.
    static std::mutex lock;
    static std::vector<std::weak_ptr<const Sampler::Sound>> made (names.size());
    static std::shared_ptr<const Sampler::Sound> first;
    const std::scoped_lock scope (lock);

    if (auto sound = made[at].lock())
        return sound;

    auto sound = make (at);
    made[at] = sound;

    if (at == 0)
        first = sound;

    return sound;
}

int getFactorySampleRoot (int index)
{
    if (index <= 0 || index >= (int) getNames().size())
        return bassRoot;

    if (index <= numSpectral)
        return spectralSounds[index - 1].root;

    const auto sound = getFactorySample (index);

    if (const auto pitch = detectPitch (sound->audio, sound->sampleRate))
        return std::clamp ((int) std::lround (*pitch), 0, 127);

    return plainRoot;
}

std::optional<std::pair<float, float>> getFactorySampleLoop (int index)
{
    if (index >= 1 && index <= numSpectral)
        return std::pair { 0.0f, 1.0f };

    return {};
}
} // namespace phazite::devices
