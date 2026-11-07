// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Oscillators.h"

namespace phazite::devices::dsp
{
const BandLimitedWaves& getBandLimitedWaves()
{
    static const BandLimitedWaves waves = []
    {
        // The plain waves' harmonics: a saw rising from -1 to 1 is -2 / (pi h) of each harmonic in sine phase; a triangle
        // from 1 down to -1 and back is 8 / (pi h)^2 of each odd one in cosine phase.
        constexpr auto pi = juce::MathConstants<double>::pi;
        Wavetable::Harmonics saw ((size_t) Wavetable::maxHarmonics), triangle ((size_t) Wavetable::maxHarmonics);

        for (int h = 1; h <= Wavetable::maxHarmonics; ++h)
        {
            saw[(size_t) h - 1] = { 0.0f, (float) (2.0 / (pi * h)) };

            if (h % 2 == 1)
                triangle[(size_t) h - 1] = { (float) (8.0 / (pi * pi * h * h)), 0.0f };
        }

        return BandLimitedWaves { Wavetable::forOscillator ("Saw", saw), Wavetable::forOscillator ("Triangle", triangle) };
    }();

    return waves;
}

Oscillator::Oscillator()
    : saw (getBandLimitedWaves().saw.get()),
      triangle (getBandLimitedWaves().triangle.get())
{
    setFrequency (0.0, 44100.0);
}
} // namespace phazite::devices::dsp
