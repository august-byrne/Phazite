// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FactoryWavetables.h"
#include "devices/dsp/Random.h"

#include <array>
#include <cmath>
#include <functional>
#include <mutex>

namespace phazite::devices
{
namespace
{
    using Harmonics = Wavetable::Harmonics;

    // Never rename one: projects keep an oscillator's table by its name.
    constexpr std::string_view names[] = { "Basic Shapes", "Pulse Width", "Saw Build", "Square Build", "Vowels", "Sync",   "FM",
                                           "FM Hollow",    "Fold",        "Digital",   "Comb",         "Growl",  "Spectra" };
    constexpr int numTables = (int) std::size (names);

    constexpr double pi = juce::MathConstants<double>::pi, twoPi = juce::MathConstants<double>::twoPi;
    constexpr int cycleSamples = 16384; // a formula's cycle, sampled finely enough that its harmonics above the table's barely fold back
    constexpr int framesPerSweep = 32;
    constexpr int spectraHarmonics = 512; // the Spectra table's highest

    // A harmonic in sine phase: amplitude * sin (h * angle).
    std::complex<float> sine (double amplitude)
    {
        return { 0.0f, (float) -amplitude };
    }

    // Harmonics with these amplitudes, in sine phase, up to the table's most.
    Harmonics withAmplitudes (const std::function<double (int harmonic)>& amplitude)
    {
        Harmonics harmonics ((size_t) Wavetable::maxHarmonics);

        for (int h = 1; h <= Wavetable::maxHarmonics; ++h)
            harmonics[(size_t) h - 1] = sine (amplitude (h));

        return harmonics;
    }

    double saw (int h)
    {
        return 1.0 / h;
    }

    double square (int h)
    {
        return h % 2 == 1 ? 1.0 / h : 0.0;
    }

    double triangle (int h)
    {
        return h % 2 == 1 ? ((h / 2) % 2 == 0 ? 1.0 : -1.0) / ((double) h * h) : 0.0;
    }

    // A frame from its wave as a formula of the angle around its cycle, sampled finely.
    std::vector<float> sampleCycle (const std::function<double (double angle)>& wave)
    {
        std::vector<float> cycle ((size_t) cycleSamples);

        for (int n = 0; n < cycleSamples; ++n)
            cycle[(size_t) n] = (float) wave (twoPi * n / cycleSamples);

        return cycle;
    }

    // Where frame f of a sweep is, from 0 at its first to 1 at its last.
    double along (int frame)
    {
        return frame / (double) (framesPerSweep - 1);
    }

    std::shared_ptr<const Wavetable> makeBasicShapes()
    {
        return Wavetable::fromHarmonics ("Basic Shapes", { withAmplitudes ([] (int h) { return h == 1 ? 1.0 : 0.0; }),
                                                           withAmplitudes (triangle), withAmplitudes (saw), withAmplitudes (square) });
    }

    // A square narrowing into a thin pulse.
    std::shared_ptr<const Wavetable> makePulseWidth()
    {
        std::vector<Harmonics> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto width = 0.5 - 0.48 * along (f);
            Harmonics harmonics ((size_t) Wavetable::maxHarmonics);

            for (int h = 1; h <= Wavetable::maxHarmonics; ++h)
            {
                // A pulse that is high for `width` of its cycle: 2 (1 - e^(-i 2 pi h width)) / (i pi h), the division by i a
                // multiplication by -i.
                const auto turn = std::polar (1.0, -twoPi * h * width);
                harmonics[(size_t) h - 1] = std::complex<float> (2.0 * (1.0 - turn) * std::complex<double> (0.0, -1.0 / (pi * h)));
            }

            frames.push_back (std::move (harmonics));
        }

        return Wavetable::fromHarmonics ("Pulse Width", frames);
    }

    // From a sine to a full saw (or square), harmonic by harmonic, the count doubling every three frames or so.
    std::shared_ptr<const Wavetable> makeBuild (const juce::String& name, double (*amplitude) (int))
    {
        std::vector<Harmonics> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto count = (int) std::lround (std::pow (2.0, 10.0 * along (f)));
            frames.push_back (withAmplitudes ([count, amplitude] (int h) { return h <= count ? amplitude (h) : 0.0; }));
        }

        return Wavetable::fromHarmonics (name, frames);
    }

    // A, E, I, O and U, gliding from one to the next: a saw's harmonics, at 110 Hz, through the vowels' first three
    // formants (Peterson and Barney's averages for men, 1952).
    std::shared_ptr<const Wavetable> makeVowels()
    {
        struct Vowel
        {
            double f1, f2, f3;
        };

        constexpr Vowel vowels[] = { { 730, 1090, 2440 },
                                     { 530, 1840, 2480 },
                                     { 270, 2290, 3010 },
                                     { 570, 840, 2410 },
                                     { 300, 870, 2240 } };
        constexpr int framesBetween = 8;
        std::vector<Harmonics> frames;

        const auto formant = [] (double frequency, double centre, double width, double gain)
        {
            const auto distance = (frequency - centre) / (0.5 * width);
            return gain / (1.0 + distance * distance);
        };

        for (size_t v = 0; v + 1 < std::size (vowels); ++v)
        {
            for (int step = 0; step < framesBetween + (v + 2 == std::size (vowels) ? 1 : 0); ++step)
            {
                // The formants glide in octaves from one vowel to the next.
                const auto x = step / (double) framesBetween;
                const auto glide = [x] (double from, double to) { return from * std::pow (to / from, x); };
                const Vowel now { glide (vowels[v].f1, vowels[v + 1].f1), glide (vowels[v].f2, vowels[v + 1].f2),
                                  glide (vowels[v].f3, vowels[v + 1].f3) };

                frames.push_back (withAmplitudes (
                    [now, &formant] (int h)
                    {
                        const auto frequency = 110.0 * h;
                        return (0.02 + formant (frequency, now.f1, 90.0, 1.0) + formant (frequency, now.f2, 110.0, 0.7)
                                + formant (frequency, now.f3, 150.0, 0.4))
                               / h;
                    }));
            }
        }

        return Wavetable::fromHarmonics ("Vowels", frames);
    }

    // A saw hard-synced to a cycle of the note, its own cycle 1 to 8 times as fast.
    std::shared_ptr<const Wavetable> makeSync()
    {
        std::vector<std::vector<float>> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto ratio = 1.0 + 7.0 * along (f);
            frames.push_back (sampleCycle (
                [ratio] (double angle)
                {
                    const auto turn = ratio * angle / twoPi;
                    return 1.0 - 2.0 * (turn - std::floor (turn));
                }));
        }

        return Wavetable::fromFrames ("Sync", frames);
    }

    // Two-operator FM: a sine whose phase another sine moves, at the same pitch or twice it (odd harmonics only, hollow),
    // ever deeper.
    std::shared_ptr<const Wavetable> makeFm (const juce::String& name, double modulatorRatio)
    {
        std::vector<std::vector<float>> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto index = 8.0 * along (f);
            frames.push_back (sampleCycle ([index, modulatorRatio] (double angle)
                                           { return std::sin (angle + index * std::sin (modulatorRatio * angle)); }));
        }

        return Wavetable::fromFrames (name, frames);
    }

    // A sine driven harder and harder into a fold, as a Buchla wavefolder does.
    std::shared_ptr<const Wavetable> makeFold()
    {
        std::vector<std::vector<float>> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto drive = 1.0 + 7.0 * along (f);
            frames.push_back (sampleCycle ([drive] (double angle) { return std::sin (0.5 * pi * drive * std::sin (angle)); }));
        }

        return Wavetable::fromFrames ("Fold", frames);
    }

    // A sine in ever fewer steps, from 128 a side down to one, as from fewer and fewer bits.
    std::shared_ptr<const Wavetable> makeDigital()
    {
        constexpr int numFrames = 16;
        std::vector<std::vector<float>> frames;

        for (int f = 0; f < numFrames; ++f)
        {
            const auto steps = std::round (std::pow (2.0, 7.0 * (1.0 - f / (double) (numFrames - 1))));
            frames.push_back (sampleCycle ([steps] (double angle) { return std::round (std::sin (angle) * steps) / steps; }));
        }

        return Wavetable::fromFrames ("Digital", frames);
    }

    // A saw through a comb filter whose notches close in, as a flanger sweeps.
    std::shared_ptr<const Wavetable> makeComb()
    {
        std::vector<Harmonics> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto delay = (1.0 / 64.0) * std::pow (16.0, along (f)); // a cycle's 1/64 to 1/4
            frames.push_back (withAmplitudes ([delay] (int h) { return std::abs (std::cos (pi * h * delay)) / h; }));
        }

        return Wavetable::fromHarmonics ("Comb", frames);
    }

    // A saw with a resonant peak moving up its harmonics, from the second to the 45th: a talking bass.
    std::shared_ptr<const Wavetable> makeGrowl()
    {
        std::vector<Harmonics> frames;

        for (int f = 0; f < framesPerSweep; ++f)
        {
            const auto centre = std::pow (2.0, 1.0 + 4.5 * along (f));
            frames.push_back (withAmplitudes (
                [centre] (int h)
                {
                    const auto octaves = std::log2 (h / centre) / 0.35;
                    return (0.1 + 2.5 * std::exp (-0.5 * octaves * octaves)) / h;
                }));
        }

        return Wavetable::fromHarmonics ("Growl", frames);
    }

    // Spectra of random harmonics, gliding from one to the next: from fixed seeds, so they are the same every time.
    std::shared_ptr<const Wavetable> makeSpectra()
    {
        constexpr int numKeys = 5, framesBetween = 8;
        std::array<std::vector<double>, numKeys> keys;

        for (int k = 0; k < numKeys; ++k)
        {
            dsp::Random random (0x5eed0u + (std::uint32_t) k);
            keys[(size_t) k].resize (spectraHarmonics);

            for (int h = 1; h <= spectraHarmonics; ++h)
                keys[(size_t) k][(size_t) h - 1] = (0.15 + 0.85 * random.nextUnipolar()) / std::pow ((double) h, 0.8);
        }

        std::vector<Harmonics> frames;

        for (int k = 0; k + 1 < numKeys; ++k)
        {
            for (int step = 0; step < framesBetween + (k + 2 == numKeys ? 1 : 0); ++step)
            {
                const auto x = step / (double) framesBetween;
                frames.push_back (withAmplitudes (
                    [&keys, k, x] (int h)
                    {
                        return h <= spectraHarmonics
                                   ? (1.0 - x) * keys[(size_t) k][(size_t) h - 1] + x * keys[(size_t) k + 1][(size_t) h - 1]
                                   : 0.0;
                    }));
            }
        }

        return Wavetable::fromHarmonics ("Spectra", frames);
    }

    std::shared_ptr<const Wavetable> make (int index)
    {
        switch (index)
        {
            case 1:
                return makePulseWidth();
            case 2:
                return makeBuild ("Saw Build", saw);
            case 3:
                return makeBuild ("Square Build", square);
            case 4:
                return makeVowels();
            case 5:
                return makeSync();
            case 6:
                return makeFm ("FM", 1.0);
            case 7:
                return makeFm ("FM Hollow", 2.0);
            case 8:
                return makeFold();
            case 9:
                return makeDigital();
            case 10:
                return makeComb();
            case 11:
                return makeGrowl();
            case 12:
                return makeSpectra();
            default:
                return makeBasicShapes();
        }
    }
} // namespace

std::span<const std::string_view> getFactoryWavetableNames()
{
    return names;
}

int findFactoryWavetable (std::string_view name)
{
    for (int i = 0; i < numTables; ++i)
        if (names[(size_t) i] == name)
            return i;

    return -1;
}

std::shared_ptr<const Wavetable> getFactoryWavetable (int index)
{
    if (! juce::isPositiveAndBelow (index, numTables))
        index = 0;

    // Each one made once, while something holds it.
    static std::mutex lock;
    static std::array<std::weak_ptr<const Wavetable>, (size_t) numTables> made;
    const std::scoped_lock scope (lock);

    if (auto table = made[(size_t) index].lock())
        return table;

    auto table = make (index);
    made[(size_t) index] = table;
    return table;
}
} // namespace phazite::devices
