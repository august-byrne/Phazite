// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FactorySamples.h"
#include "devices/drums/DrumSynthesis.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>
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

    const std::vector<std::string_view>& getNames()
    {
        static const std::vector<std::string_view> names = []
        {
            std::vector<std::string_view> made { bassName };
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
        else
        {
            const auto drum = getFactoryDrum ((int) index - 1);
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

    const auto sound = getFactorySample (index);

    if (const auto pitch = detectPitch (sound->audio, sound->sampleRate))
        return std::clamp ((int) std::lround (*pitch), 0, 127);

    return plainRoot;
}
} // namespace phazite::devices
