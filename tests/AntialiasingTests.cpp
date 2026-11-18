// SPDX-License-Identifier: AGPL-3.0-or-later
// What the DSP audit (docs/archive/dsp-audit.md) fixed, measured: the band-limited oscillator's waves (src/devices/dsp/Oscillators.h) and
// the synth's basic waves and pulse on its tables, as clean (src/devices/wavetable/Wavetable.h), the
// saturator and the drives played by their antiderivatives at twice the rate (src/devices/dsp/Saturation.h), the EQ's
// matched bands (src/devices/dsp/Filters.h), the delay line's Lagrange interpolation (src/devices/dsp/DelayLine.h) and the
// drum sampler's windowed sinc (src/devices/dsp/SincReader.h). Aliases are measured as the reference measurements of the
// audit were: a tone a whole number of FFT bins, so that every true harmonic lands on a multiple of its bin, and everything
// else in the audible band is aliasing.
#include "TestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/FactoryWavetables.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <functional>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace dsp = phazite::devices::dsp;
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr int fftOrder = 16, fftSize = 1 << fftOrder;

std::vector<float> getDefaults (const devices::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, const devices::DeviceInfo& info, std::string_view id, float value)
{
    const auto index = devices::findParameter (info, id);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

// A frequency near the one asked for that is a whole, odd number of bins, which no alias of its harmonics lands on a multiple of.
double onBin (double frequency, double sampleRate)
{
    auto bin = (int) std::lround (frequency * fftSize / sampleRate);

    if (bin % 2 == 0)
        ++bin;

    return bin * sampleRate / fftSize;
}

// How far the loudest alias in the audible band (up to 20 kHz) is under the tone, in decibels, in fftSize samples of a sound.
double getAliasesUnder (const float* sound, double frequency, double sampleRate)
{
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) fftSize * 2, 0.0f);
    std::copy (sound, sound + fftSize, data.begin());
    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    const auto bin = (int) std::lround (frequency * fftSize / sampleRate);
    const auto tone = (double) data[(size_t) bin];
    auto loudest = 1.0e-12;

    for (int k = 1; k < fftSize / 2 && k * sampleRate / fftSize < 20000.0; ++k)
        if (k % bin != 0)
            loudest = std::max (loudest, (double) data[(size_t) k]);

    return 20.0 * std::log10 (tone / loudest);
}

// Plays a processor in blocks of 480 samples on a mono sound (on both channels) and returns its left channel.
std::vector<float> play (devices::Processor& processor, const std::vector<float>& values, double sampleRate, int numSamples,
                         const std::function<float (int)>& input)
{
    constexpr int blockSize = 480;
    processor.prepare (sampleRate, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> played;

    for (int done = 0; done < numSamples; done += blockSize)
    {
        const auto length = std::min (blockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
        {
            const auto sample = input (done + i);
            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = values;
        processor.process (context);

        for (int i = 0; i < length; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}

// A tone's level in a stretch of a sound, by least squares, as a gain.
double getLevel (const float* sound, int numSamples, double frequency, double sampleRate)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto s = std::sin (twoPi * frequency * i / sampleRate), c = std::cos (twoPi * frequency * i / sampleRate);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += sound[i] * s;
        yc += sound[i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    const auto a = (ys * cc - yc * sc) / determinant, b = (yc * ss - ys * sc) / determinant;
    return std::sqrt (a * a + b * b);
}

// A note's frequency (A3, A5, A6 or A7) and a sample rate within a tenth of a percent of 48 kHz at which it is a whole, odd number
// of bins: an oscillator's step, that many bins over fftSize, is then exact in floats, as the synth works its
// frequency out from the note, so that what is measured is the oscillator's aliases alone, not a tone between two bins.
struct OnBin
{
    int note = 69;
    double frequency = 440.0, rate = 48000.0;
};

OnBin onBinRate (int note)
{
    const auto frequency = 440.0 * std::exp2 ((note - 69) / 12.0);
    auto bin = (int) std::lround (frequency * fftSize / 48000.0);

    if (bin % 2 == 0)
        ++bin;

    return { note, frequency, frequency * fftSize / bin };
}

constexpr int measuredNotes[] = { 57, 81, 93, 105 }; // 220, 880, 1760 and 3520 Hz, the oscillator's own measurement's
constexpr int lowNotes[] = { 21, 33, 45 };           // 27.5, 55 and 110 Hz: the lowest read the tables' levels that are capped

// The synth's first oscillator alone at full level, on a table at a position: the filter off, no modulation, no
// velocity, full sustain. A note held from the start, its left side from 4800 samples on, fftSize of them.
std::vector<float> playWavetable (const OnBin& at, float position, std::shared_ptr<const devices::Wavetable> table = nullptr)
{
    using devices::Synth;
    auto values = getDefaults (Synth::info);
    values[Synth::osc1Position] = position;
    values[Synth::osc1Level] = 1.0f;
    values[Synth::filterOn] = 0.0f;
    values[Synth::mod1Source] = 0.0f;
    values[Synth::env1Sustain] = 1.0f;
    values[Synth::velocity] = 0.0f;
    values[Synth::level] = 0.0f;

    constexpr int blockSize = 480, skipped = 4800;
    Synth synth;
    synth.prepare (at.rate, blockSize);

    if (table != nullptr)
        synth.setTable (0, table);

    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> played;
    const devices::NoteEvent start { devices::NoteEvent::Type::noteOn, 0, at.note, 1.0f };

    for (int done = 0; done < skipped + fftSize; done += blockSize)
    {
        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = blockSize;
        context.parameters = values;

        if (done == 0)
            context.events = std::span (&start, 1);

        synth.process (context);
        played.insert (played.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    played.erase (played.begin(), played.begin() + skipped);
    played.resize ((size_t) fftSize);
    return played;
}

// The band-limited oscillator (the sub's) at a note, fftSize samples of it.
std::vector<float> playOscillator (const OnBin& at, dsp::Waveform wave, float width = 0.5f)
{
    dsp::Oscillator oscillator;
    oscillator.setFrequency (at.frequency, at.rate);
    std::vector<float> sound ((size_t) fftSize);

    for (auto& sample : sound)
        sample = oscillator.next (wave, width);

    return sound;
}

} // namespace

TEST_CASE (
    "The synth's basic saw, square and triangle, and its 25 % pulse, keep their aliases as far down as the band-limited oscillator's")
{
    // The Basic Shapes table's saw, square and triangle (at 67 %, 100 % and 33 % of it) and the Pulse Width table at 25 %,
    // against the band-limited oscillator's waves at the same notes. Kept with four samples for each cycle of a level's highest
    // harmonic and read by cubic interpolation, their images left the saw's loudest alias 56 dB under the note at A6, 31 dB
    // above the oscillator's; with sixteen, they are more than 90 dB under at every note, and never more than 3 dB above it.
    const auto basic = devices::getFactoryWavetable (0);
    const auto pulses = devices::getFactoryWavetable (devices::findFactoryWavetable ("Pulse Width"));

    struct Wave
    {
        dsp::Waveform wave;
        float width, position;
        std::shared_ptr<const devices::Wavetable> table;
    };

    const Wave waves[] = { { dsp::Waveform::saw, 0.5f, 2.0f / 3.0f, basic },
                           { dsp::Waveform::square, 0.5f, 1.0f, basic },
                           { dsp::Waveform::triangle, 0.5f, 1.0f / 3.0f, basic },
                           { dsp::Waveform::square, 0.25f, (0.5f - 0.25f) / 0.48f, pulses } }; // that table's width at 25 %

    std::vector<int> notes (std::begin (lowNotes), std::end (lowNotes));
    notes.insert (notes.end(), std::begin (measuredNotes), std::end (measuredNotes));

    for (const auto note : notes)
    {
        const auto at = onBinRate (note);

        for (const auto& wave : waves)
        {
            const auto synth = playOscillator (at, wave.wave, wave.width);
            const auto table = playWavetable (at, wave.position, wave.table);
            const auto synthAliases = getAliasesUnder (synth.data(), at.frequency, at.rate);
            const auto tableAliases = getAliasesUnder (table.data(), at.frequency, at.rate);
            CAPTURE (note);
            CAPTURE ((int) wave.wave);
            CAPTURE (wave.width);
            CAPTURE (synthAliases);
            CAPTURE (tableAliases);
            CHECK (tableAliases > 90.0);
            CHECK (tableAliases > synthAliases - 3.0);
        }
    }
}

TEST_CASE ("The band-limited oscillator's saw, pulse and triangle keep their aliases out of hearing, up to A7")
{
    // PolyBLEP's saw had its loudest alias 37 dB under the note at A5 and 25 dB at A7, and the triangle's was 34 dB under at
    // A7 (docs/archive/dsp-audit.md, 5.3). Read from band-limited tables, every harmonic that plays aliases above 20 kHz.
    constexpr double rate = 48000.0;

    for (const auto note : { 220.0, 880.0, 1760.0, 3520.0 })
    {
        for (const auto wave : { dsp::Waveform::saw, dsp::Waveform::square, dsp::Waveform::triangle })
        {
            for (const auto width : { 0.5f, 0.25f })
            {
                if (wave != dsp::Waveform::square && width < 0.5f)
                    continue;

                const auto frequency = onBin (note, rate);
                dsp::Oscillator oscillator;
                oscillator.setFrequency (frequency, rate);
                std::vector<float> sound ((size_t) fftSize);

                for (auto& sample : sound)
                    sample = oscillator.next (wave, width);

                CAPTURE (note);
                CAPTURE ((int) wave);
                CAPTURE (width);
                CHECK (getAliasesUnder (sound.data(), frequency, rate) > 78.0);
            }
        }
    }
}

TEST_CASE ("The pulse wave is as far above the middle as below it, at any width: no offset under it")
{
    // High for its width at 1 and low for the rest at -1, it carried an offset of twice its width less 1 (-0.75 at 12.5 %),
    // which an envelope made a thump of at each note's start and end. Its swing from low to high is still 2, around its mean.
    constexpr double rate = 48000.0;

    for (const auto width : { 0.05f, 0.125f, 0.25f, 0.5f, 0.75f, 0.95f })
    {
        for (const auto samplesPerCycle : { 480, 96, 16 }) // 100 Hz, 500 Hz and 3 kHz: whole cycles in what is measured
        {
            dsp::Oscillator oscillator;
            oscillator.setFrequency (rate / samplesPerCycle, rate);
            auto sum = 0.0;
            auto lowest = 0.0f, highest = 0.0f;

            for (int i = 0; i < 48000; ++i)
            {
                const auto sample = oscillator.next (dsp::Waveform::square, width);
                sum += sample;
                lowest = std::min (lowest, sample);
                highest = std::max (highest, sample);
            }

            CAPTURE (width);
            CAPTURE (samplesPerCycle);
            CHECK (std::abs (sum / 48000.0) < 0.002);

            if (samplesPerCycle == 480) // low enough for the wave's flat parts to show (with the ripple of its last harmonics)
                CHECK (highest - lowest == doctest::Approx (2.0f).epsilon (0.15));
        }
    }
}

TEST_CASE ("The saturator, driven hard, keeps its aliases down, the more the higher its Quality, and what it leaves straight comes "
           "out level to 20 kHz")
{
    // How far under the tone its loudest alias stays at each Quality, at the worst of a 5 and a 12 kHz tone driven 24 dB into
    // Hard, Fold and Tube, at 44.1 and 48 kHz (docs/design/devices/saturator.md gives these figures). Each Quality holds to
    // its figure, less 3 dB, and does better than the one below it.
    constexpr double figures[] = { 15.4, 36.9, 61.8 }; // measured 2026-11-18: 15.41, 36.91, 61.76
    double previous = 0.0;

    for (int quality = 0; quality < 3; ++quality)
    {
        auto worst = 1000.0;

        for (const auto rate : { 44100.0, 48000.0 })
        {
            for (const auto hertz : { 5000.0, 12000.0 })
            {
                for (const auto shape :
                     { devices::Saturator::Shape::hard, devices::Saturator::Shape::fold, devices::Saturator::Shape::tube })
                {
                    auto values = getDefaults (devices::Saturator::info);
                    set (values, devices::Saturator::info, "drive", 24.0f);
                    set (values, devices::Saturator::info, "shape", (float) shape);
                    set (values, devices::Saturator::info, "quality", (float) quality);
                    const auto frequency = onBin (hertz, rate);
                    devices::Saturator saturator;
                    const auto played = play (saturator, values, rate, fftSize + 4800,
                                              [&] (int t) { return 0.5f * (float) std::sin (twoPi * frequency * t / rate); });
                    const auto under = getAliasesUnder (played.data() + 4800, frequency, rate);
                    MESSAGE ("Quality " << quality << ", " << rate << " Hz, " << hertz << " Hz, shape " << (int) shape << ": " << under
                                        << " dB");
                    worst = std::min (worst, under);
                }
            }
        }

        MESSAGE ("Quality " << quality << ": aliases at least " << worst << " dB under the tone");
        CAPTURE (quality);
        CHECK (worst > figures[quality] - 3.0);

        if (quality > 0)
            CHECK (worst > previous);

        previous = worst;
    }

    // At the default Quality, 2x, a tone lower in the band, 2.5 kHz driven 24 dB into each, keeps its aliases much further
    // down: held to the figure it measures, less 3 dB.
    {
        constexpr double figure = 50.2; // measured 2026-11-18: 50.21, the worst of the six
        auto worst = 1000.0;

        for (const auto rate : { 44100.0, 48000.0 })
        {
            for (const auto shape : { devices::Saturator::Shape::hard, devices::Saturator::Shape::fold, devices::Saturator::Shape::tube })
            {
                auto values = getDefaults (devices::Saturator::info);
                set (values, devices::Saturator::info, "drive", 24.0f);
                set (values, devices::Saturator::info, "shape", (float) shape);
                const auto frequency = onBin (2500.0, rate);
                devices::Saturator saturator;
                const auto played = play (saturator, values, rate, fftSize + 4800,
                                          [&] (int t) { return 0.5f * (float) std::sin (twoPi * frequency * t / rate); });
                worst = std::min (worst, getAliasesUnder (played.data() + 4800, frequency, rate));
            }
        }

        MESSAGE ("Quality 2x, 2.5 kHz: aliases at least " << worst << " dB under the tone");
        CHECK (worst > figure - 3.0);
    }

    // A quiet tone, which the curve leaves straight: as loud out as in, at 15 and 20 kHz too (the averaging alone dulled 20
    // kHz by 2.4 dB at twice 44.1 kHz).
    for (const auto rate : { 44100.0, 48000.0 })
    {
        for (const auto hertz : { 1000.0, 15000.0, 20000.0 })
        {
            auto values = getDefaults (devices::Saturator::info);
            set (values, devices::Saturator::info, "drive", 0.0f);
            devices::Saturator saturator;
            const auto played =
                play (saturator, values, rate, 9600, [&] (int t) { return 0.001f * (float) std::sin (twoPi * hertz * t / rate); });

            CAPTURE (rate);
            CAPTURE (hertz);
            CHECK (std::abs (juce::Decibels::gainToDecibels (getLevel (played.data() + 4800, 4800, hertz, rate) / 0.001)) < 0.35);
        }
    }
}

TEST_CASE ("The saturator's Quality, automated from 2x to 8x and back while it plays, keeps its aliases down as when set by hand")
{
    // A 12 kHz tone at 44.1 kHz driven 24 dB into Fold, the worst case of 2x and 4x: in each stretch the aliases are as far
    // under the tone as on a saturator set to that Quality from the start. All three Qualities come equally late, so that
    // automation can reach each from any.
    constexpr double rate = 44100.0;
    constexpr int stretch = fftSize + 9600, settle = 9600, blockSize = 480;
    const auto frequency = onBin (12000.0, rate);
    const auto tone = [&] (int t) { return 0.5f * (float) std::sin (twoPi * frequency * t / rate); };
    const auto at = [] (float quality)
    {
        auto values = getDefaults (devices::Saturator::info);
        set (values, devices::Saturator::info, "drive", 24.0f);
        set (values, devices::Saturator::info, "shape", (float) devices::Saturator::Shape::fold);
        set (values, devices::Saturator::info, "quality", quality);
        return values;
    };

    const auto twice = at (0.0f), eightTimes = at (2.0f);
    devices::Saturator setTwice, setEightTimes;
    const auto byHandTwice = getAliasesUnder (play (setTwice, twice, rate, settle + fftSize, tone).data() + settle, frequency, rate);
    const auto byHandEightTimes =
        getAliasesUnder (play (setEightTimes, eightTimes, rate, settle + fftSize, tone).data() + settle, frequency, rate);
    REQUIRE (byHandEightTimes > byHandTwice + 20.0);

    // 2x, then 8x, then 2x again, each a stretch long, the values changing as a block starts.
    devices::Saturator saturator;
    saturator.prepare (rate, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> played;

    for (int done = 0; done < 3 * stretch; done += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, tone (done + i));
            buffer.setSample (1, i, tone (done + i));
        }

        const auto& values = (done / stretch) == 1 ? eightTimes : twice;
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = blockSize;
        context.parameters = values;
        saturator.process (context);
        played.insert (played.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    for (int part = 0; part < 3; ++part)
    {
        const auto under = getAliasesUnder (played.data() + part * stretch + settle, frequency, rate);
        const auto byHand = part == 1 ? byHandEightTimes : byHandTwice;
        CAPTURE (part);
        CAPTURE (under);
        CAPTURE (byHand);
        CHECK (std::abs (under - byHand) < 1.0);
    }
}

TEST_CASE ("The filter's drive keeps its aliases down, and undriven the filter is as it was, only later")
{
    constexpr double rate = 48000.0;
    const auto frequency = onBin (3000.0, rate);
    auto values = getDefaults (devices::FilterEffect::info);
    set (values, devices::FilterEffect::info, "drive", 24.0f);
    devices::FilterEffect filter;
    const auto played =
        play (filter, values, rate, fftSize + 4800, [&] (int t) { return 0.5f * (float) std::sin (twoPi * frequency * t / rate); });
    CHECK (getAliasesUnder (played.data() + 4800, frequency, rate) > 50.0);

    // Undriven and open, a tone passes at its level.
    set (values, devices::FilterEffect::info, "drive", 0.0f);
    devices::FilterEffect open;
    const auto quiet = play (open, values, rate, 9600, [&] (int t) { return 0.25f * (float) std::sin (twoPi * 5000.0 * t / rate); });
    CHECK (std::abs (juce::Decibels::gainToDecibels (getLevel (quiet.data() + 4800, 4800, 5000.0, rate) / 0.25)) < 0.35);
    CHECK (open.getLatencySamples() > 0);
}

TEST_CASE ("The multiband's saturation keeps its aliases down, and while nothing saturates a limited peak stays where it was")
{
    constexpr double rate = 48000.0;
    const auto frequency = onBin (3000.0, rate);
    auto values = getDefaults (devices::Multiband::info);

    // The firmer curve from 24 dB under a ceiling of -12 dB: a tone at -6 dBFS flattens on it.
    for (const auto* band : { "low", "mid", "high" })
    {
        set (values, devices::Multiband::info, std::string (band) + "Saturation", 1.0f);
        set (values, devices::Multiband::info, std::string (band) + "Ceiling", -12.0f);
    }

    devices::Multiband multiband;
    const auto played =
        play (multiband, values, rate, fftSize + 9600, [&] (int t) { return 0.5f * (float) std::sin (twoPi * frequency * t / rate); });
    CHECK (getAliasesUnder (played.data() + 9600, frequency, rate) > 40.0);

    // None saturating, the sound passes the bands as it did, only as late as their saturations would make it.
    auto plain = getDefaults (devices::Multiband::info);
    devices::Multiband unsaturated;
    const auto quiet = play (unsaturated, plain, rate, 9600, [&] (int t) { return 0.25f * (float) std::sin (twoPi * 1000.0 * t / rate); });
    CHECK (std::abs (juce::Decibels::gainToDecibels (getLevel (quiet.data() + 4800, 4800, 1000.0, rate) / 0.25)) < 0.1);
}

TEST_CASE ("The EQ's bands keep their analog shape near the top, where the bilinear transform squeezed them")
{
    // A bell at 15 kHz, a high shelf at 12 kHz and a low shelf cut at 8 kHz, at 48 kHz: matched, within 1 dB of the analog
    // band across the audible range; bilinear, the bell was more than 3 dB off.
    constexpr double rate = 48000.0;

    struct Band
    {
        dsp::FilterShape shape;
        double frequency, q, gain;
    };

    const Band bands[] = { { dsp::FilterShape::bell, 15000.0, 1.0, 12.0 },
                           { dsp::FilterShape::highShelf, 12000.0, 0.7, 6.0 },
                           { dsp::FilterShape::lowShelf, 8000.0, 0.7, -6.0 },
                           { dsp::FilterShape::lowPass, 16000.0, 0.7071, 0.0 } };

    for (const auto& band : bands)
    {
        const auto matched = dsp::makeMatchedFilter (band.shape, band.frequency, rate, band.q, band.gain);
        const auto bilinear = dsp::makeFilter (band.shape, band.frequency, rate, band.q, band.gain);
        auto matchedWorst = 0.0, bilinearWorst = 0.0;

        for (auto hertz = 20.0; hertz <= 20000.0; hertz *= 1.05)
        {
            const auto analog = std::abs (dsp::getAnalogResponse (band.shape, hertz, band.frequency, band.q, band.gain));
            const auto toDecibels = [&] (const dsp::FilterCoefficients& c)
            { return 20.0 * std::log10 (dsp::getMagnitude (c, hertz, rate) / analog); };
            matchedWorst = std::max (matchedWorst, std::abs (toDecibels (matched)));
            bilinearWorst = std::max (bilinearWorst, std::abs (toDecibels (bilinear)));
        }

        CAPTURE ((int) band.shape);
        CAPTURE (matchedWorst);
        CAPTURE (bilinearWorst);
        CHECK (matchedWorst < 1.0);
        CHECK (matchedWorst < bilinearWorst);
    }

    // A notch is still a notch, and a flat bell is no filter at all.
    const auto notch = dsp::makeMatchedFilter (dsp::FilterShape::notch, 10000.0, rate, 4.0);
    CHECK (dsp::getMagnitude (notch, 10000.0, rate) < 0.01);
    CHECK (dsp::getMagnitude (notch, 1000.0, rate) == doctest::Approx (1.0).epsilon (0.01));
    const auto flat = dsp::makeMatchedFilter (dsp::FilterShape::bell, 5000.0, rate, 1.0, 0.0);
    CHECK (dsp::getMagnitude (flat, 17000.0, rate) == doctest::Approx (1.0).epsilon (1.0e-4));

    // The EQ draws what it plays.
    auto values = getDefaults (devices::Equalizer::info);
    set (values, devices::Equalizer::info, "band3Frequency", 15000.0f);
    set (values, devices::Equalizer::info, "band3Gain", 12.0f);
    set (values, devices::Equalizer::info, "band3Q", 1.0f);
    const auto shown = devices::Equalizer::getMagnitude (values, 13000.0, rate);
    const auto made = dsp::getMagnitude (dsp::makeMatchedFilter (dsp::FilterShape::bell, 15000.0, rate, 1.0, 12.0), 13000.0, rate);
    CHECK (shown == doctest::Approx (made).epsilon (1.0e-4));
}

TEST_CASE ("The delay's echoes keep their treble at a delay between samples")
{
    // 44 ms at 44.1 kHz is 1940.4 samples. Read linearly, each pass lost 2.3 dB at 10 kHz, 16 dB by the eighth echo; read by
    // 4-point Lagrange interpolation, 0.7 dB, so that the eighth echo is within 6 dB at 10 kHz of what the feedback and the
    // cuts leave at 1 kHz.
    constexpr double rate = 44100.0;
    auto values = getDefaults (devices::Delay::info);
    set (values, devices::Delay::info, "sync", 0.0f);
    set (values, devices::Delay::info, "time", 44.0f);
    set (values, devices::Delay::info, "feedback", 0.8f);
    set (values, devices::Delay::info, "lowCut", 20.0f);
    set (values, devices::Delay::info, "highCut", 20000.0f);
    devices::Delay delay;
    const auto played = play (delay, values, rate, 22050, [] (int t) { return t == 0 ? 0.5f : 0.0f; });

    // Each echo's level at a frequency, from 512 samples around where it comes back.
    const auto levelOf = [&] (int echo, double hertz)
    {
        const auto at = (int) std::lround (echo * 0.044 * rate) - 256;
        juce::dsp::FFT fft (9);
        std::vector<float> data (1024, 0.0f);
        std::copy (played.begin() + at, played.begin() + at + 512, data.begin());
        fft.performFrequencyOnlyForwardTransform (data.data(), true);
        return (double) data[(size_t) std::lround (hertz * 512 / rate)];
    };

    const auto trebleLost = 20.0 * std::log10 ((levelOf (8, 10000.0) / levelOf (1, 10000.0)) / (levelOf (8, 1000.0) / levelOf (1, 1000.0)));
    CAPTURE (trebleLost);
    CHECK (trebleLost > -6.0);
}

TEST_CASE ("A drum pad at another rate keeps its top octave, and one tuned up leaves out what would fold back")
{
    // A 15 kHz sine sampled at 44.1 kHz, on a pad played at 48 kHz: the 4-point Hermite read lost 1.3 dB of it.
    constexpr double rate = 48000.0;
    auto sound = std::make_shared<devices::DrumSampler::Sound>();
    sound->sampleRate = 44100.0;
    sound->audio.setSize (1, 44100);

    for (int i = 0; i < 44100; ++i)
        sound->audio.setSample (0, i, 0.5f * (float) std::sin (twoPi * 15000.0 * i / 44100.0));

    const auto playPad = [&] (float tune)
    {
        devices::DrumSampler sampler;
        sampler.prepare (rate, 480);
        sampler.setSound (0, sound);
        auto values = getDefaults (devices::DrumSampler::info);
        values[(size_t) devices::DrumSampler::getParameterIndex (0, devices::DrumSampler::tune)] = tune;
        juce::AudioBuffer<float> buffer (2, 480);
        std::vector<float> played;

        for (int done = 0; done < 24000; done += 480)
        {
            buffer.clear();
            const devices::NoteEvent hit { devices::NoteEvent::Type::noteOn, 0, devices::DrumSampler::firstNote, 1.0f };
            float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
            devices::ProcessContext context;
            context.channels = channels;
            context.numChannels = 2;
            context.numSamples = 480;
            context.parameters = values;

            if (done == 0)
                context.events = std::span (&hit, 1);

            sampler.process (context);
            played.insert (played.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 480);
        }

        return played;
    };

    const auto atOwnPitch = playPad (0.0f);
    const auto level = getLevel (atOwnPitch.data() + 2000, 8000, 15000.0, rate);
    CHECK (std::abs (juce::Decibels::gainToDecibels (level / 0.5)) < 0.2);

    // An octave up, it would be 30 kHz, above the 24 kHz the engine plays: nothing of it comes back below.
    const auto octaveUp = playPad (12.0f);
    auto loudest = 0.0f;

    for (size_t i = 2000; i < 10000; ++i)
        loudest = std::max (loudest, std::abs (octaveUp[i]));

    CHECK (loudest < 0.5f * 0.001f); // 60 dB down
}
