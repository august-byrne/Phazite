// SPDX-License-Identifier: AGPL-3.0-or-later
// Sounds made from their spectra (src/devices/dsp/SpectralLoop.h, the PADsynth algorithm): the loop is the same from the same
// seed and another from another, at an RMS of 1; each harmonic is a band with the harmonic's power, as wide as asked, in
// cents up the harmonics or in hertz as the scale says; the loop has no seam; half-way round, it is unrelated to itself;
// and harmonics past half the sample rate, or none at all, are left out.
#include "TestHelpers.h"
#include "devices/dsp/SpectralLoop.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace
{
namespace dsp = phazite::devices::dsp;
constexpr double testRate = 48000.0;
constexpr int testOrder = 16; // 65536 samples: 1.4 s, 0.73 Hz between its frequencies

// The power at each of a loop's frequencies: the loop is one period, so its transform holds all of it.
std::vector<double> getPowers (const std::vector<float>& loop)
{
    const auto size = (int) loop.size();
    const auto order = (int) std::lround (std::log2 ((double) size));
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    std::copy (loop.begin(), loop.end(), data.begin());
    fft.performRealOnlyForwardTransform (data.data(), true);
    std::vector<double> powers ((size_t) size / 2);

    for (size_t bin = 0; bin < powers.size(); ++bin)
        powers[bin] = (double) data[bin * 2] * data[bin * 2] + (double) data[bin * 2 + 1] * data[bin * 2 + 1];

    return powers;
}

// A band around a frequency: its power, and its power's standard deviation, in hertz.
struct Band
{
    double power = 0.0, deviation = 0.0;
};

Band measureBand (const std::vector<double>& powers, double centre, double reach)
{
    const auto binHertz = testRate / (2.0 * (double) powers.size());
    auto total = 0.0, weighted = 0.0, spread = 0.0;

    for (auto bin = (int) std::ceil ((centre - reach) / binHertz); bin <= (int) std::floor ((centre + reach) / binHertz); ++bin)
    {
        total += powers[(size_t) bin];
        weighted += powers[(size_t) bin] * bin * binHertz;
    }

    const auto mean = weighted / total;

    for (auto bin = (int) std::ceil ((centre - reach) / binHertz); bin <= (int) std::floor ((centre + reach) / binHertz); ++bin)
        spread += powers[(size_t) bin] * (bin * binHertz - mean) * (bin * binHertz - mean);

    return { total, std::sqrt (spread / total) };
}

double rmsOf (const std::vector<float>& loop)
{
    return std::sqrt (std::inner_product (loop.begin(), loop.end(), loop.begin(), 0.0) / (double) loop.size());
}

// A band's width in hertz as its bandwidth in cents makes it: the power's standard deviation is a quarter of it.
double deviationFor (double bandwidth, double hertz)
{
    return 0.25 * (std::exp2 (bandwidth / 1200.0) - 1.0) * hertz;
}
} // namespace

TEST_CASE ("A spectral loop is the same from the same seed, another from another, and at an RMS of 1")
{
    const std::vector<float> harmonics { 1.0f, 0.5f, 0.3333f, 0.25f };
    const dsp::SpectralLoop shape { .harmonics = harmonics, .fundamental = 220.0, .bandwidth = 20.0, .order = testOrder, .seed = 7 };
    const auto first = dsp::makeSpectralLoop (shape);
    const auto again = dsp::makeSpectralLoop (shape);
    REQUIRE (first.size() == (size_t) 1 << testOrder);
    CHECK (first == again);
    CHECK (rmsOf (first) == doctest::Approx (1.0).epsilon (1.0e-4));

    auto other = shape;
    other.seed = 8;
    const auto different = dsp::makeSpectralLoop (other);
    CHECK (first != different);
    CHECK (rmsOf (different) == doctest::Approx (1.0).epsilon (1.0e-4));

    // The same spectrum, whatever the phases: each harmonic's power is the same in both.
    const auto powers = getPowers (first), otherPowers = getPowers (different);

    for (int harmonic = 1; harmonic <= 4; ++harmonic)
        CHECK (measureBand (powers, 220.0 * harmonic, 20.0).power
               == doctest::Approx (measureBand (otherPowers, 220.0 * harmonic, 20.0).power).epsilon (1.0e-3));

    // Its peaks are where random phases put them: about 12 dB over its RMS, and nowhere near a sum of sines in phase.
    const auto peak =
        std::abs (*std::max_element (first.begin(), first.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }));
    CHECK (peak > 2.0f);
    CHECK (peak < 6.0f);
}

TEST_CASE ("A spectral loop's harmonics have their levels, each in a band as wide as asked, in cents up the harmonics")
{
    // A saw's first twelve harmonics, at 220 Hz, each band 24 cents wide.
    std::vector<float> harmonics;

    for (int harmonic = 1; harmonic <= 12; ++harmonic)
        harmonics.push_back (1.0f / (float) harmonic);

    const auto loop =
        dsp::makeSpectralLoop ({ .harmonics = harmonics, .fundamental = 220.0, .bandwidth = 24.0, .order = testOrder, .seed = 3 });
    const auto powers = getPowers (loop);
    const auto first = measureBand (powers, 220.0, 12.0);

    for (int harmonic = 1; harmonic <= 12; ++harmonic)
    {
        CAPTURE (harmonic);
        const auto centre = 220.0 * harmonic;
        const auto band = measureBand (powers, centre, 12.0 * harmonic);

        // Its power against the first's: 1/n^2, whatever its width.
        CHECK (10.0 * std::log10 (band.power / first.power) == doctest::Approx (-20.0 * std::log10 ((double) harmonic)).epsilon (0.01));

        // Its power's standard deviation: a quarter of the bandwidth, the same in cents for every harmonic.
        CHECK (band.deviation == doctest::Approx (deviationFor (24.0, centre)).epsilon (0.03));
    }

    // Nothing between the bands: all of the loop's power is in them.
    auto inBands = 0.0;

    for (int harmonic = 1; harmonic <= 12; ++harmonic)
        inBands += measureBand (powers, 220.0 * harmonic, 12.0 * harmonic).power;

    CHECK (inBands == doctest::Approx (std::accumulate (powers.begin(), powers.end(), 0.0)).epsilon (1.0e-4));
}

TEST_CASE ("A spectral loop's bandwidth scale keeps the bands as wide in hertz at 0, and widens them faster past 1")
{
    const std::vector<float> harmonics { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    const auto deviationOf = [&harmonics] (double scale, int harmonic)
    {
        const auto loop = dsp::makeSpectralLoop (
            { .harmonics = harmonics, .fundamental = 330.0, .bandwidth = 30.0, .bandwidthScale = scale, .order = testOrder, .seed = 11 });
        return measureBand (getPowers (loop), 330.0 * harmonic, 100.0).deviation;
    };

    const auto base = deviationFor (30.0, 330.0);
    CHECK (deviationOf (0.0, 1) == doctest::Approx (base).epsilon (0.04));
    CHECK (deviationOf (0.0, 8) == doctest::Approx (base).epsilon (0.04));
    CHECK (deviationOf (1.0, 8) == doctest::Approx (8.0 * base).epsilon (0.04));
    CHECK (deviationOf (0.5, 4) == doctest::Approx (2.0 * base).epsilon (0.04));
    CHECK (deviationOf (1.5, 4) == doctest::Approx (8.0 * base).epsilon (0.04));
}

TEST_CASE ("A spectral loop has no seam, and half-way round it is unrelated to itself")
{
    std::vector<float> harmonics;

    for (int harmonic = 1; harmonic <= 100; ++harmonic)
        harmonics.push_back (1.0f / (float) harmonic);

    const auto loop =
        dsp::makeSpectralLoop ({ .harmonics = harmonics, .fundamental = 220.0, .bandwidth = 19.14, .order = testOrder, .seed = 5 });
    const auto size = loop.size();

    // From its last sample to its first, no more of a step than anywhere in it.
    auto largest = 0.0f;

    for (size_t i = 1; i < size; ++i)
        largest = std::max (largest, std::abs (loop[i] - loop[i - 1]));

    CHECK (std::abs (loop[0] - loop[size - 1]) <= largest);

    // And as smooth there as anywhere: the second difference across the seam is no larger than within.
    auto sharpest = 0.0f;

    for (size_t i = 2; i < size; ++i)
        sharpest = std::max (sharpest, std::abs (loop[i] - 2.0f * loop[i - 1] + loop[i - 2]));

    CHECK (std::abs (loop[1] - 2.0f * loop[0] + loop[size - 1]) <= sharpest);
    CHECK (std::abs (loop[0] - 2.0f * loop[size - 1] + loop[size - 2]) <= sharpest);

    // The loop against itself from half-way round: two sides of a wide sound.
    auto both = 0.0;

    for (size_t i = 0; i < size; ++i)
        both += (double) loop[i] * loop[(i + size / 2) % size];

    CHECK (std::abs (both / (double) size) < 0.05);
}

TEST_CASE ("A spectral loop leaves out harmonics past half the sample rate, and is silent with none")
{
    // Harmonics of 5 kHz: the first four are under 24 kHz, the rest are left out.
    const std::vector<float> harmonics (16, 1.0f);
    const auto loop = dsp::makeSpectralLoop ({ .harmonics = harmonics, .fundamental = 5000.0, .bandwidth = 10.0, .order = 14, .seed = 2 });
    const auto powers = getPowers (loop);
    const auto total = std::accumulate (powers.begin(), powers.end(), 0.0);
    auto inFour = 0.0;

    for (int harmonic = 1; harmonic <= 4; ++harmonic)
        inFour += measureBand (powers, 5000.0 * harmonic, 400.0).power;

    CHECK (inFour == doctest::Approx (total).epsilon (1.0e-4));

    for (const auto sample : loop)
        REQUIRE (std::isfinite (sample));

    // A band too narrow for the loop to hold is its nearest frequencies, at the harmonic's power all the same.
    const std::vector<float> two { 1.0f, 0.5f };
    const auto narrow = getPowers (dsp::makeSpectralLoop ({ .harmonics = two, .fundamental = 1000.0, .bandwidth = 0.0, .order = 12 }));
    CHECK (measureBand (narrow, 2000.0, 30.0).power / measureBand (narrow, 1000.0, 30.0).power == doctest::Approx (0.25).epsilon (0.01));

    const auto silent = dsp::makeSpectralLoop ({ .harmonics = {}, .order = 12 });
    REQUIRE (silent.size() == 4096);
    CHECK (std::all_of (silent.begin(), silent.end(), [] (float sample) { return juce::exactlyEqual (sample, 0.0f); }));

    const std::vector<float> none { 0.0f, 0.0f };
    const auto alsoSilent = dsp::makeSpectralLoop ({ .harmonics = none, .order = 12 });
    CHECK (std::all_of (alsoSilent.begin(), alsoSilent.end(), [] (float sample) { return juce::exactlyEqual (sample, 0.0f); }));
}
