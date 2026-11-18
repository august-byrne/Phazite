// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's synth (src/devices/synth/) and its tables (src/devices/wavetable/): the tables, kept band-limited at every
// pitch and read from sounds as Serum writes them, its factory tables, and the synth itself: its pitch and velocity, its
// position moving through the table (by hand and by an envelope for each note), its warps, unison (with its stack and its
// drift), sub, noise, filter (its slopes, combs, notch, drive and what moves its cutoff), pitch bend, glides and slides, the
// sustain pedal and voices (stolen, a key played again, as it rings out, while it is held or under the pedal, each note on
// a voice of its own), that it doesn't alias, that its warps leave nothing off centre (with what a part of a frame's cycle
// adds up to), that it lets go of the tables it no longer plays only once no control step reads them, and that it plays the
// same every time.
#include "TestHelpers.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dynamics/TruePeak.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/Wavetable.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::NoteEvent;
using devices::Synth;
using devices::Wavetable;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// The first oscillator alone, on its table at full level: the filter off, no modulation, no velocity, full sustain.
std::vector<float> getPlainValues (float position)
{
    auto values = getDefaults();
    values[Synth::osc1Position] = position;
    values[Synth::osc1Level] = 1.0f;
    values[Synth::filterOn] = 0.0f;
    values[Synth::mod1Source] = 0.0f;
    values[Synth::env1Sustain] = 1.0f;
    values[Synth::velocity] = 0.0f;
    values[Synth::level] = 0.0f;
    return values;
}

NoteEvent noteOn (int sample, int note, float velocity = 1.0f)
{
    return { NoteEvent::Type::noteOn, sample, note, velocity };
}

NoteEvent noteOff (int sample, int note)
{
    return { NoteEvent::Type::noteOff, sample, note, 0.0f };
}

struct Played
{
    std::vector<float> left, right;
    std::vector<int> voicesAfter; // sounding after each block
};

// Plays the synth in blocks (of testBlockSize samples unless given), with events at their samples from the start. The song
// plays in the blocks that begin before playingUntil.
Played play (Synth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
             int blockSize = testBlockSize, int playingUntil = 0)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    Played played;
    std::vector<NoteEvent> inBlock;

    for (int done = 0; done < numSamples; done += blockSize)
    {
        const auto length = std::min (blockSize, numSamples - done);
        buffer.clear();
        inBlock.clear();

        for (auto event : events)
        {
            if (event.sample >= done && event.sample < done + length)
            {
                event.sample -= done;
                inBlock.push_back (event);
            }
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        block.events = inBlock;
        block.isPlaying = done < playingUntil;
        synth.process (block);
        played.voicesAfter.push_back (synth.getActiveVoices());

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

std::unique_ptr<Synth> makeSynth()
{
    auto synth = std::make_unique<Synth>();
    synth->prepare (testRate, testBlockSize);
    return synth;
}

// The frequency of a steady tone between two samples, from its rising zero crossings.
double measureFrequency (const std::vector<float>& sound, size_t from, size_t to)
{
    size_t first = 0, last = 0;
    auto crossings = 0;

    for (auto i = from + 1; i < to; ++i)
    {
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
        {
            if (crossings == 0)
                first = i;

            last = i;
            ++crossings;
        }
    }

    return crossings > 1 ? (crossings - 1) * testRate / (double) (last - first) : 0.0;
}

// How strong a frequency is in a stretch of sound, as an amplitude (Goertzel, through a Hann window).
double measureLevelAt (const std::vector<float>& sound, size_t from, size_t to, double frequency)
{
    const auto coefficient = 2.0 * std::cos (twoPi * frequency / testRate);
    const auto length = (double) (to - from);
    auto s1 = 0.0, s2 = 0.0, weights = 0.0;

    for (auto i = from; i < to; ++i)
    {
        const auto weight = 0.5 - 0.5 * std::cos (twoPi * (double) (i - from) / length);
        const auto s0 = weight * sound[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
        weights += weight;
    }

    const auto power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt (std::max (power, 0.0)) / weights;
}

double noteFrequency (int note)
{
    return 440.0 * std::pow (2.0, (note - 69) / 12.0);
}

// A level's harmonic h, as an amplitude, by a DFT of its cycle.
double measureHarmonic (const Wavetable::Level& level, int harmonic)
{
    auto re = 0.0, im = 0.0;

    for (int n = 0; n < level.length; ++n)
    {
        const auto angle = twoPi * harmonic * n / level.length;
        re += level.samples[n] * std::cos (angle);
        im -= level.samples[n] * std::sin (angle);
    }

    return 2.0 * std::sqrt (re * re + im * im) / level.length;
}

// A saw's harmonics, in sine phase.
Wavetable::Harmonics makeSaw()
{
    Wavetable::Harmonics harmonics ((size_t) Wavetable::maxHarmonics);

    for (int h = 1; h <= Wavetable::maxHarmonics; ++h)
        harmonics[(size_t) h - 1] = { 0.0f, -1.0f / (float) h };

    return harmonics;
}

float peakOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < to && i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// The largest step from one sample to the next.
float largestStep (const std::vector<float>& sound, size_t from, size_t to)
{
    auto largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < to && i < sound.size(); ++i)
        largest = std::max (largest, std::abs (sound[i] - sound[i - 1]));

    return largest;
}

// The largest difference between two sounds, on either side.
float largestDifference (const Played& a, const Played& b)
{
    auto largest = 0.0f;

    for (size_t i = 0; i < a.left.size() && i < b.left.size(); ++i)
        largest = std::max ({ largest, std::abs (a.left[i] - b.left[i]), std::abs (a.right[i] - b.right[i]) });

    return largest;
}

NoteEvent pedal (int sample, bool down)
{
    return { NoteEvent::Type::controller, sample, 64, down ? 1.0f : 0.0f };
}

// What a new synth plays of some events, and what new synths play of each group of them alone, added: a key played again
// plays each of its notes as it would alone, what rings out of the one before untouched and the new one from its start, as
// a note on a voice of its own does.
std::pair<Played, Played> playApart (const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& together,
                                     const std::vector<std::vector<NoteEvent>>& groups)
{
    auto played = play (*makeSynth(), values, numSamples, together);
    Played added;
    added.left.assign (played.left.size(), 0.0f);
    added.right.assign (played.right.size(), 0.0f);

    for (const auto& group : groups)
    {
        const auto part = play (*makeSynth(), values, numSamples, group);

        for (size_t i = 0; i < part.left.size(); ++i)
        {
            added.left[i] += part.left[i];
            added.right[i] += part.right[i];
        }
    }

    return { std::move (played), std::move (added) };
}
} // namespace

TEST_CASE ("The synth's description holds together")
{
    REQUIRE (devices::isValid (Synth::info));
    CHECK (Synth::info.kind == devices::DeviceKind::instrument);
    CHECK (Synth::info.name == std::string_view ("Synth"));
    CHECK (Synth::info.parameters.size() == (size_t) Synth::quality + 1);
    CHECK (devices::findParameter (Synth::info, "osc2Position") == Synth::osc2Position);
    CHECK (devices::findParameter (Synth::info, "mod8Amount") == Synth::mod8Amount);
    CHECK (devices::findParameter (Synth::info, "voiceMode") == Synth::voiceMode);
}

TEST_CASE ("A wavetable keeps each frame band-limited at every level, half an octave apart")
{
    const auto table = Wavetable::fromHarmonics ("Saw", { makeSaw() });
    REQUIRE (table->getNumFrames() == 1);

    for (int level = 0; level < Wavetable::numLevels; ++level)
    {
        const auto harmonics = Wavetable::getHarmonics (level);
        const auto wave = table->getLevel (0, level);
        INFO ("level " << level << ", " << harmonics << " harmonics");
        REQUIRE (wave.length >= 4 * harmonics);
        CHECK (juce::isPowerOfTwo (wave.length));

        // Its highest harmonic is there, as loud against the first as in a saw; nothing above it is.
        const auto first = measureHarmonic (wave, 1);
        CHECK (measureHarmonic (wave, harmonics) / first == doctest::Approx (1.0 / harmonics).epsilon (0.01));

        for (const auto above : { harmonics + 1, harmonics * 2 + 1, wave.length / 2 - 1 })
            if (above < wave.length / 2)
                CHECK (measureHarmonic (wave, above) < 1.0e-4 * first);

        // Each cycle wraps around for interpolation.
        CHECK (wave.samples[-1] == doctest::Approx (wave.samples[wave.length - 1]));
        CHECK (wave.samples[wave.length] == doctest::Approx (wave.samples[0]));
        CHECK (wave.samples[wave.length + 1] == doctest::Approx (wave.samples[1]));
    }

    CHECK (Wavetable::getHarmonics (0) == Wavetable::maxHarmonics);
    CHECK (Wavetable::getHarmonics (Wavetable::numLevels - 1) == 1);

    // The level for a pitch: the most harmonics that stay at or below the highest that may play.
    CHECK (Wavetable::chooseLevel (2000.0) == 0);
    CHECK (Wavetable::chooseLevel (1024.0) == 0);
    CHECK (Wavetable::chooseLevel (1023.0) == 1);
    CHECK (Wavetable::chooseLevel (100.0) == 7); // 90 harmonics
    CHECK (Wavetable::chooseLevel (1.0) == 19);
    CHECK (Wavetable::chooseLevel (0.5) == Wavetable::numLevels - 1);

    // The loudest frame's peak is 1.
    auto peak = 0.0f;

    for (const auto sample : table->getFrameForDrawing (0))
        peak = std::max (peak, std::abs (sample));

    CHECK (peak == doctest::Approx (1.0f));
}

TEST_CASE ("A wavetable reads a sound as Serum does: whole frames, a single cycle, or the frame length its 'clm ' chunk gives")
{
    // Four frames of 2048 samples, each a sine of a harmonic more.
    std::vector<float> frames;

    for (int frame = 0; frame < 4; ++frame)
        for (int n = 0; n < Wavetable::frameSize; ++n)
            frames.push_back ((float) std::sin (twoPi * (frame + 1) * n / Wavetable::frameSize));

    const auto table = Wavetable::fromSound ("Four", frames);
    REQUIRE (table->getNumFrames() == 4);
    CHECK (table->getName() == juce::String ("Four"));

    for (int frame = 0; frame < 4; ++frame)
    {
        const auto wave = table->getLevel (frame, 0);
        INFO ("frame " << frame);
        CHECK (measureHarmonic (wave, frame + 1) > 0.9);
        CHECK (measureHarmonic (wave, frame + 2) < 0.01);
    }

    // Serum frames of 1024 (from a 'clm ' chunk): eight, each a whole cycle of its own.
    std::vector<float> shorter;

    for (int frame = 0; frame < 8; ++frame)
        for (int n = 0; n < 1024; ++n)
            shorter.push_back ((float) std::sin (twoPi * (frame + 1) * n / 1024));

    const auto serum = Wavetable::fromSound ("Serum", shorter, 1024);
    REQUIRE (serum->getNumFrames() == 8);
    CHECK (measureHarmonic (serum->getLevel (7, 0), 8) > 0.9);
    CHECK (measureHarmonic (serum->getLevel (7, 0), 1) < 0.01);

    // A single cycle of any length, as many single-cycle waves are.
    std::vector<float> cycle;

    for (int n = 0; n < 600; ++n)
        cycle.push_back (0.5f * (float) std::sin (twoPi * 3 * n / 600));

    const auto single = Wavetable::fromSound ("Single", cycle);
    REQUIRE (single->getNumFrames() == 1);
    CHECK (measureHarmonic (single->getLevel (0, 0), 3) > 0.9); // made as loud as it goes
    CHECK (measureHarmonic (single->getLevel (0, 0), 1) < 0.01);

    // Any other sound: its first whole frames of 2048.
    std::vector<float> longer (3 * Wavetable::frameSize + 100, 0.25f);
    CHECK (Wavetable::fromSound ("Longer", longer)->getNumFrames() == 3);

    // No sound: one silent frame.
    const auto silent = Wavetable::fromSound ("Silent", {});
    REQUIRE (silent->getNumFrames() == 1);
    CHECK (measureHarmonic (silent->getLevel (0, 0), 1) == doctest::Approx (0.0));
}

TEST_CASE ("The synth's factory tables have names of their own and make themselves once")
{
    const auto names = devices::getFactoryWavetableNames();
    REQUIRE (names.size() >= 12);
    std::set<std::string_view> seen;

    for (int index = 0; index < (int) names.size(); ++index)
    {
        const auto name = names[(size_t) index];
        INFO (std::string (name));
        CHECK (! name.empty());
        CHECK (seen.insert (name).second);
        CHECK (devices::findFactoryWavetable (name) == index);

        const auto table = devices::getFactoryWavetable (index);
        REQUIRE (table != nullptr);
        CHECK (table->getName() == juce::String (name.data(), name.size()));
        CHECK (table->getNumFrames() >= 4);

        // Shared while it is held.
        CHECK (devices::getFactoryWavetable (index) == table);
    }

    CHECK (names[0] == std::string_view ("Basic Shapes"));
    CHECK (devices::findFactoryWavetable ("No Such Table") == -1);
    CHECK (devices::getFactoryWavetable (-1) == devices::getFactoryWavetable (0));
}

TEST_CASE ("A synth note plays at its pitch, the sine at the table's start")
{
    auto synth = makeSynth();
    const auto values = getPlainValues (0.0f); // Basic Shapes' sine
    const auto played = play (*synth, values, 24000, { noteOn (0, 69) });

    CHECK (measureFrequency (played.left, 4800, 24000) == doctest::Approx (440.0).epsilon (0.002));
    CHECK (synth->getActiveVoices() == 1);

    // A sine: nothing at its second or third harmonics.
    const auto fundamental = measureLevelAt (played.left, 4800, 24000, 440.0);
    CHECK (fundamental > 0.5);
    CHECK (measureLevelAt (played.left, 4800, 24000, 880.0) < 0.001 * fundamental);
    CHECK (measureLevelAt (played.left, 4800, 24000, 1320.0) < 0.001 * fundamental);

    // In the middle, without unison.
    CHECK (played.left == played.right);
}

TEST_CASE ("The synth's position morphs through its table, and an envelope moves it for each note")
{
    // Saw Build: a sine at its start, a full saw at its end.
    const auto build = devices::getFactoryWavetable (devices::findFactoryWavetable ("Saw Build"));
    const auto fifthAt =
        [&build] (float position, const std::vector<NoteEvent>& events, size_t from, size_t to, std::vector<float> values = {})
    {
        auto synth = makeSynth();
        synth->setTable (0, build);

        if (values.empty())
            values = getPlainValues (position);

        const auto played = play (*synth, values, (int) to, events);
        return measureLevelAt (played.left, from, to, 5.0 * noteFrequency (45))
               / measureLevelAt (played.left, from, to, noteFrequency (45));
    };

    CHECK (fifthAt (0.0f, { noteOn (0, 45) }, 4800, 14400) < 0.001);
    CHECK (fifthAt (1.0f, { noteOn (0, 45) }, 4800, 14400) == doctest::Approx (0.2).epsilon (0.05)); // a saw's fifth is a fifth as loud

    // Envelope 2 lifts the position to the table's end at each note's start, then lets it fall back as it decays.
    auto values = getPlainValues (0.0f);
    values[Synth::mod1Source] = (float) Synth::ModSource::envelope2;
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod1Amount] = 1.0f;
    values[Synth::env2Attack] = 0.0f;
    values[Synth::env2Decay] = 200.0f;
    values[Synth::env2Sustain] = 0.0f;
    const std::vector<NoteEvent> events { noteOn (0, 45), noteOff (24000, 45), noteOn (28800, 45) };

    CHECK (fifthAt (0.0f, events, 0, 1200, values) > 0.1);       // bright as it starts
    CHECK (fifthAt (0.0f, events, 14400, 24000, values) < 0.01); // dark once it has decayed
    CHECK (fifthAt (0.0f, events, 28800, 30000, values) > 0.1);  // bright again with the next note
}

TEST_CASE ("The synth doesn't alias, however high it plays")
{
    // Basic Shapes' saw, high: its harmonics that would fold back below 20 kHz are left out.
    for (const auto note : { 84, 96, 105 })
    {
        auto synth = makeSynth();
        const auto played = play (*synth, getPlainValues (2.0f / 3.0f), 19200, { noteOn (0, note) });
        const auto f0 = noteFrequency (note);
        const auto fundamental = measureLevelAt (played.left, 4800, 19200, f0);
        INFO ("note " << note);
        REQUIRE (fundamental > 0.1);

        // Between the harmonics, below 20 kHz: nothing (-80 dB).
        for (auto frequency = 0.5 * f0; frequency < 20000.0; frequency += f0)
            CHECK (measureLevelAt (played.left, 4800, 19200, frequency) < 1.0e-4 * fundamental);
    }
}

TEST_CASE ("The synth's warps reshape its wave, and sync's jump is smoothed")
{
    // Each warp at 70 % changes the sine at the table's start, without blowing up; FM and ring modulation need the other
    // oscillator.
    const auto plain = play (*makeSynth(), getPlainValues (0.0f), 9600, { noteOn (0, 57) });

    for (const auto warp : { Synth::Warp::sync, Synth::Warp::bendUp, Synth::Warp::bendDown, Synth::Warp::pulseWidth, Synth::Warp::mirror,
                             Synth::Warp::fm, Synth::Warp::ringMod })
    {
        auto values = getPlainValues (0.0f);
        values[Synth::osc1Warp] = (float) warp;
        values[Synth::osc1WarpAmount] = 0.7f;
        values[Synth::osc2On] = 1.0f;
        values[Synth::osc2Level] = 0.0f; // heard only through the warps
        values[Synth::osc2Pitch] = 12.0f;
        values[Synth::osc2Position] = 0.0f;
        const auto warped = play (*makeSynth(), values, 9600, { noteOn (0, 57) });
        INFO ("warp " << (int) warp);

        auto difference = 0.0f, peak = 0.0f;

        for (size_t i = 4800; i < 9600; ++i)
        {
            difference = std::max (difference, std::abs (warped.left[i] - plain.left[i]));
            peak = std::max (peak, std::abs (warped.left[i]));
        }

        CHECK (difference > 0.1f);
        CHECK (peak < 2.0f);
        CHECK (std::isfinite (peak));
    }

    // Hard sync at an octave and a half: the sine's harmonics pile up, but nothing folds back between them (-60 dB below
    // the strongest).
    auto values = getPlainValues (0.0f);
    values[Synth::osc1Warp] = (float) Synth::Warp::sync;
    values[Synth::osc1WarpAmount] = 0.5f; // 2^1.5 times as fast
    const auto synced = play (*makeSynth(), values, 19200, { noteOn (0, 81) });
    const auto f0 = noteFrequency (81);
    auto strongest = 0.0;

    for (int harmonic = 1; harmonic <= 8; ++harmonic)
        strongest = std::max (strongest, measureLevelAt (synced.left, 4800, 19200, harmonic * f0));

    REQUIRE (strongest > 0.1);

    for (auto frequency = 0.5 * f0; frequency < 18000.0; frequency += f0)
        CHECK (measureLevelAt (synced.left, 4800, 19200, frequency) < 1.0e-3 * strongest);
}

TEST_CASE ("The synth's unison spreads its voices across the stereo field and its table")
{
    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::unisonVoices] = 3.0f; // four
    values[Synth::unisonDetune] = 30.0f;
    values[Synth::unisonSpread] = 1.0f;
    const auto spread = play (*makeSynth(), values, 9600, { noteOn (0, 57) });

    auto difference = 0.0f;

    for (size_t i = 0; i < spread.left.size(); ++i)
        difference = std::max (difference, std::abs (spread.left[i] - spread.right[i]));

    CHECK (difference > 0.05f);

    // About as loud as one voice: their level is the square root of their number down.
    const auto rms = [] (const std::vector<float>& sound)
    {
        auto sum = 0.0;

        for (size_t i = 4800; i < sound.size(); ++i)
            sum += (double) sound[i] * sound[i];

        return std::sqrt (sum / (double) (sound.size() - 4800));
    };

    const auto single = play (*makeSynth(), getPlainValues (2.0f / 3.0f), 9600, { noteOn (0, 57) });
    CHECK (rms (spread.left) == doctest::Approx (rms (single.left)).epsilon (0.35));

    // Spread across the table: with its position spread, its voices play different frames of Saw Build, from its start to
    // its end, so the 60th harmonic, which its middle frame hasn't, is heard.
    values[Synth::unisonSpread] = 0.0f;
    values[Synth::unisonDetune] = 0.0f;
    values[Synth::osc1Position] = 0.5f;
    auto synth = makeSynth();
    synth->setTable (0, devices::getFactoryWavetable (devices::findFactoryWavetable ("Saw Build")));
    const auto still = play (*synth, values, 9600, { noteOn (0, 45) });
    values[Synth::unisonPosition] = 1.0f;
    auto spreadSynth = makeSynth();
    spreadSynth->setTable (0, devices::getFactoryWavetable (devices::findFactoryWavetable ("Saw Build")));
    const auto moved = play (*spreadSynth, values, 9600, { noteOn (0, 45) });
    const auto sixtieth = 60.0 * noteFrequency (45);
    CHECK (measureLevelAt (still.left, 4800, 9600, sixtieth) < 1.0e-4);
    CHECK (measureLevelAt (moved.left, 4800, 9600, sixtieth) > 1.0e-3);
}

namespace
{
// A steady tone's pitch in each stretch of `window` samples from `from`, in cents against a frequency: from its rising zero
// crossings, placed between the samples.
std::vector<double> measureCents (const std::vector<float>& sound, size_t from, size_t window, double reference, double rate = testRate)
{
    std::vector<double> cents;

    for (auto start = from; start + window <= sound.size(); start += window)
    {
        auto first = 0.0, last = 0.0;
        auto crossings = 0;

        for (auto i = start + 1; i < start + window; ++i)
        {
            if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
            {
                const auto place = (double) (i - 1) + (double) sound[i - 1] / ((double) sound[i - 1] - (double) sound[i]);

                if (crossings == 0)
                    first = place;

                last = place;
                ++crossings;
            }
        }

        cents.push_back (crossings > 1 ? 1200.0 * std::log2 ((crossings - 1) * rate / (last - first) / reference) : 0.0);
    }

    return cents;
}

double getLargest (const std::vector<double>& values)
{
    auto largest = 0.0;

    for (const auto value : values)
        largest = std::max (largest, std::abs (value));

    return largest;
}

double getRange (const std::vector<double>& values)
{
    const auto [lowest, highest] = std::minmax_element (values.begin(), values.end());
    return values.empty() ? 0.0 : *highest - *lowest;
}

// The table's sine, with unison copies in the middle, none detuned.
std::vector<float> getStackValues (Synth::Stack stack, int copies)
{
    auto values = getPlainValues (0.0f);
    values[Synth::unisonVoices] = (float) (copies - 1);
    values[Synth::unisonDetune] = 0.0f;
    values[Synth::unisonSpread] = 0.0f;
    values[Synth::unisonStack] = (float) stack;
    return values;
}
} // namespace

TEST_CASE ("The synth's unison stack puts every second or third copy above the note, each as loud")
{
    using Stack = Synth::Stack;
    CHECK (juce::exactlyEqual (Synth::info.parameters[Synth::unisonStack].defaultValue, 0.0f));
    CHECK (devices::findParameter (Synth::info, "unisonStack") == Synth::unisonStack);
    const auto f0 = noteFrequency (57);

    // One copy plays the note whatever the stack: the table's sine, at its own level, with nothing above it.
    const auto off = play (*makeSynth(), getStackValues (Stack::off, 1), 19200, { noteOn (0, 57) });
    const auto alone = play (*makeSynth(), getStackValues (Stack::twoOctaves, 1), 19200, { noteOn (0, 57) });
    const auto sine = measureLevelAt (off.left, 4800, 19200, f0);
    REQUIRE (sine > 0.5);
    CHECK (measureLevelAt (off.left, 4800, 19200, 2.0 * f0) < 0.002);
    CHECK (alone.left == off.left);

    struct Expected
    {
        Stack stack;
        int copies;
        std::vector<int> semitones;
    };

    // As many copies as the stack has steps: one on each, in the stack's order.
    for (const auto& expected : { Expected { Stack::octave, 2, { 0, 12 } }, Expected { Stack::twoOctaves, 3, { 0, 12, 24 } },
                                  Expected { Stack::fifth, 2, { 0, 7 } }, Expected { Stack::powerChord, 3, { 0, 7, 12 } } })
    {
        INFO ("stack " << (int) expected.stack);
        const auto steps = Synth::getStackSteps (expected.stack);
        REQUIRE (steps.size() == expected.semitones.size());
        const auto played = play (*makeSynth(), getStackValues (expected.stack, expected.copies), 19200, { noteOn (0, 57) });

        for (size_t step = 0; step < steps.size(); ++step)
        {
            CHECK (juce::exactlyEqual (steps[step], (float) expected.semitones[step]));
            CHECK (measureLevelAt (played.left, 4800, 19200, f0 * std::pow (2.0, expected.semitones[step] / 12.0))
                   == doctest::Approx (sine / std::sqrt ((double) expected.copies)).epsilon (0.03));
        }

        // The sine itself has nothing up there.
        CHECK (measureLevelAt (played.left, 4800, 19200, 3.0 * f0) < 0.002);
        CHECK (played.left == played.right);
    }

    // Eight copies 20 cents apart take the stack's steps in turn: the first, fourth and seventh on the note, the second, fifth
    // and eighth an octave up, the third and sixth two octaves up.
    auto values = getStackValues (Stack::twoOctaves, 8);
    values[Synth::unisonDetune] = 70.0f;
    const auto eight = play (*makeSynth(), values, 96000, { noteOn (0, 69) });
    const auto levelAt = [&eight] (int copy, int octaves)
    {
        const auto cents = -70.0 + 20.0 * copy + 1200.0 * octaves;
        return measureLevelAt (eight.left, 24000, 96000, 440.0 * std::pow (2.0, cents / 1200.0));
    };

    for (int copy = 0; copy < 8; ++copy)
    {
        INFO ("copy " << copy);

        for (int octaves = 0; octaves < 3; ++octaves)
        {
            if (copy % 3 == octaves)
                CHECK (levelAt (copy, octaves) == doctest::Approx (sine / std::sqrt (8.0)).epsilon (0.05));
            else
                CHECK (levelAt (copy, octaves) < 0.02);
        }
    }
}

TEST_CASE ("A stacked unison's copies on the note keep the harmonics those above it can't play, and none aliases")
{
    // Basic Shapes' saw at C6, a copy on the note, one an octave up and one two octaves up, which has room for five harmonics
    // only: the copy on the note still plays its eleventh, as loud as without the stack.
    auto values = getPlainValues (2.0f / 3.0f);
    const auto f0 = noteFrequency (84);
    const auto single = play (*makeSynth(), values, 19200, { noteOn (0, 84) });
    values[Synth::unisonVoices] = 2.0f; // three
    values[Synth::unisonDetune] = 0.0f;
    values[Synth::unisonSpread] = 0.0f;
    values[Synth::unisonStack] = (float) Synth::Stack::twoOctaves;
    const auto stacked = play (*makeSynth(), values, 19200, { noteOn (0, 84) });

    const auto eleventh = measureLevelAt (single.left, 4800, 19200, 11.0 * f0);
    REQUIRE (eleventh > 0.02);
    CHECK (measureLevelAt (stacked.left, 4800, 19200, 11.0 * f0) == doctest::Approx (eleventh / std::sqrt (3.0)).epsilon (0.05));

    // Every copy's harmonics are the note's: between them, below 20 kHz, nothing (-80 dB).
    const auto fundamental = measureLevelAt (stacked.left, 4800, 19200, f0);
    REQUIRE (fundamental > 0.1);

    for (auto frequency = 0.5 * f0; frequency < 20000.0; frequency += f0)
        CHECK (measureLevelAt (stacked.left, 4800, 19200, frequency) < 1.0e-4 * fundamental);

    // At the top of the keyboard, eight copies of both oscillators stacked and drifting as far as they go stay in bounds.
    values = getDefaults();
    values[Synth::osc2On] = 1.0f;
    values[Synth::osc2Pitch] = 36.0f;
    values[Synth::unisonVoices] = 7.0f;
    values[Synth::unisonDetune] = 100.0f;
    values[Synth::unisonStack] = (float) Synth::Stack::twoOctaves;
    values[Synth::unisonDrift] = 50.0f;
    const auto extreme = play (*makeSynth(), values, 24000, { noteOn (0, 127), noteOn (0, 0), noteOn (4800, 60) });

    for (const auto sample : extreme.left)
    {
        REQUIRE (std::isfinite (sample));
        REQUIRE (std::abs (sample) < 4.0f);
    }
}

TEST_CASE ("The synth's unison drift lets each copy's pitch wander on its own, no further than its cents")
{
    CHECK (juce::exactlyEqual (Synth::info.parameters[Synth::unisonDrift].defaultValue, 0.0f));
    constexpr size_t window = 4800; // 100 ms

    // One copy of the table's sine: without drift it stays on the note.
    auto values = getPlainValues (0.0f);
    const auto steady = measureCents (play (*makeSynth(), values, 96000, { noteOn (0, 69) }).left, window, window, 440.0);
    CHECK (getLargest (steady) < 0.2);

    // Drifting up to 30 cents, it wanders around the note, never further, and doesn't settle.
    values[Synth::unisonDrift] = 30.0f;
    const auto drifting = play (*makeSynth(), values, 480000, { noteOn (0, 69) });
    const auto wander = measureCents (drifting.left, window, window, 440.0);
    CHECK (getLargest (wander) <= 30.0);
    CHECK (getRange (wander) > 20.0);

    for (size_t start = 0; start + 20 <= wander.size(); start += 20)
        CHECK (getRange ({ wander.begin() + (std::ptrdiff_t) start, wander.begin() + (std::ptrdiff_t) start + 20 }) > 1.0); // each 2 s

    // Half as far at half the amount.
    values[Synth::unisonDrift] = 15.0f;
    const auto half = measureCents (play (*makeSynth(), values, 480000, { noteOn (0, 69) }).left, window, window, 440.0);

    for (size_t i = 0; i < wander.size(); ++i)
        CHECK (half[i] == doctest::Approx (0.5 * wander[i]).epsilon (0.02).scale (1.0));

    // Two copies, one on each side: each on its own way, at its own pace.
    values[Synth::unisonDrift] = 30.0f;
    values[Synth::unisonVoices] = 1.0f;
    values[Synth::unisonDetune] = 0.0f;
    values[Synth::unisonSpread] = 1.0f;
    const auto two = play (*makeSynth(), values, 480000, { noteOn (0, 69) });
    const auto leftWander = measureCents (two.left, window, window, 440.0), rightWander = measureCents (two.right, window, window, 440.0);
    CHECK (getLargest (leftWander) <= 30.0);
    CHECK (getLargest (rightWander) <= 30.0);
    auto apart = 0.0;

    for (size_t i = 0; i < leftWander.size(); ++i)
        apart = std::max (apart, std::abs (leftWander[i] - rightWander[i]));

    CHECK (apart > 15.0);

    // Both oscillators' copies wander apart too: two sines on one note, which without drift would hold their level, beat.
    values = getPlainValues (0.0f);
    values[Synth::osc2On] = 1.0f;
    values[Synth::osc2Position] = 0.0f;
    values[Synth::osc2Level] = 1.0f;
    const auto level = [] (const std::vector<float>& sound)
    {
        std::vector<double> levels;

        for (size_t start = 4800; start + 2400 <= sound.size(); start += 2400)
        {
            auto sum = 0.0;

            for (auto i = start; i < start + 2400; ++i)
                sum += (double) sound[i] * sound[i];

            levels.push_back (std::sqrt (sum / 2400.0));
        }

        return levels;
    };

    const auto together = level (play (*makeSynth(), values, 240000, { noteOn (0, 69) }).left);
    values[Synth::unisonDrift] = 30.0f;
    const auto beating = level (play (*makeSynth(), values, 240000, { noteOn (0, 69) }).left);
    CHECK (getRange (together) < 0.01);
    CHECK (getRange (beating) > 0.5);

    // Each note from somewhere of its own: the same key six times, each once the last has rung out, starts in six places.
    values = getPlainValues (0.0f);
    values[Synth::unisonDrift] = 30.0f;
    values[Synth::env1Release] = 20.0f;
    std::vector<NoteEvent> repeats;

    for (int i = 0; i < 6; ++i)
    {
        repeats.push_back (noteOn (i * 24000, 69));
        repeats.push_back (noteOff (i * 24000 + 12000, 69));
    }

    const auto repeated = play (*makeSynth(), values, 6 * 24000, repeats).left;
    std::vector<double> starts;

    for (int i = 0; i < 6; ++i)
    {
        // Over its first 50 ms, from when its attack is through.
        const auto from = repeated.begin() + i * 24000 + 240;
        const auto start = measureCents ({ from, from + 2400 }, 0, 2400, 440.0);
        REQUIRE (start.size() == 1);
        starts.push_back (start[0]);
    }

    std::sort (starts.begin(), starts.end());
    CHECK (starts.back() - starts.front() > 10.0);

    for (size_t i = 1; i < starts.size(); ++i)
        CHECK (starts[i] - starts[i - 1] > 0.05);

    // And the same every time.
    values = getDefaults();
    values[Synth::osc2On] = 1.0f;
    values[Synth::unisonVoices] = 7.0f;
    values[Synth::unisonStack] = (float) Synth::Stack::twoOctaves;
    values[Synth::unisonDrift] = 25.0f;
    const std::vector<NoteEvent> notes { noteOn (0, 48), noteOn (3000, 55), noteOff (9000, 48), noteOn (12000, 60), noteOn (15000, 55) };
    const auto once = play (*makeSynth(), values, 24000, notes);
    const auto again = play (*makeSynth(), values, 24000, notes);
    CHECK (once.left == again.left);
    CHECK (once.right == again.right);
}

TEST_CASE ("The synth's unison drift keeps its time at any sample rate, and however blocks split the samples")
{
    // One copy of the table's sine wandering up to 30 cents, over four seconds: where it is each 100 ms, at 48 kHz.
    auto values = getPlainValues (0.0f);
    values[Synth::unisonDrift] = 30.0f;

    const auto wanderAt = [&values] (double rate)
    {
        Synth synth;
        synth.prepare (rate, testBlockSize);
        const auto played = play (synth, values, (int) std::lround (4.0 * rate), { noteOn (0, 69) });
        const auto window = (size_t) std::lround (0.1 * rate);
        return measureCents (played.left, window, window, 440.0, rate);
    };

    const auto reference = wanderAt (48000.0);
    REQUIRE (getRange (reference) > 10.0);

    // The same way at the other rates: its places and its pace are set in seconds.
    for (const auto rate : { 44100.0, 96000.0, 192000.0 })
    {
        INFO ("rate " << rate);
        const auto wander = wanderAt (rate);
        REQUIRE (wander.size() == reference.size());
        CHECK (getLargest (wander) <= 30.0);

        for (size_t i = 0; i < wander.size(); ++i)
            CHECK (std::abs (wander[i] - reference[i]) < 2.0);
    }

    // A stacked, drifting unison of both oscillators, a note held and another that comes and goes: in the usual blocks and in
    // blocks of 37 samples, the same.
    values = getPlainValues (2.0f / 3.0f);
    values[Synth::osc2On] = 1.0f;
    values[Synth::osc2Fine] = 7.0f;
    values[Synth::unisonVoices] = 5.0f;
    values[Synth::unisonStack] = (float) Synth::Stack::powerChord;
    values[Synth::unisonDrift] = 20.0f;
    const std::vector<NoteEvent> events { noteOn (0, 60), noteOn (1000, 67), noteOff (5003, 67) };
    const auto steady = play (*makeSynth(), values, 24000, events);
    const auto split = play (*makeSynth(), values, 24000, events, 37);
    auto largest = 0.0f, loudest = 0.0f;

    for (size_t i = 0; i < steady.left.size(); ++i)
    {
        largest = std::max ({ largest, std::abs (steady.left[i] - split.left[i]), std::abs (steady.right[i] - split.right[i]) });
        loudest = std::max (loudest, std::abs (steady.left[i]));
    }

    CHECK (loudest > 0.1f);
    CHECK (largest < 1.0e-4f);
}

TEST_CASE ("A stack chosen and a drift turned up while a note sounds leave no step in its wave")
{
    // Two copies of the table's sine, held: stacked an octave from one block to the next, the copy that moves up goes on from
    // where its wave was; and drifting from the next, both do.
    auto synth = makeSynth();
    auto values = getStackValues (Synth::Stack::off, 2);
    auto sound = play (*synth, values, 9600, { noteOn (0, 57) }).left;
    values[Synth::unisonStack] = (float) Synth::Stack::octave;
    const auto stacked = play (*synth, values, 9600, {}).left;
    values[Synth::unisonDrift] = 50.0f;
    const auto drifting = play (*synth, values, 9600, {}).left;
    sound.insert (sound.end(), stacked.begin(), stacked.end());
    sound.insert (sound.end(), drifting.begin(), drifting.end());

    // The copy that moved plays an octave up, as loud as the one that stayed.
    const auto f0 = noteFrequency (57);
    const auto level = measureLevelAt (sound, 14400, 19200, 2.0 * f0);
    CHECK (level > 0.3);
    CHECK (measureLevelAt (sound, 14400, 19200, f0) == doctest::Approx (level).epsilon (0.03));

    // The steepest two sines an octave apart, each at that level, get from one sample to the next.
    const auto steepest = (float) (level * twoPi * 3.0 * f0 / testRate);

    for (size_t i = 4800; i < sound.size(); ++i)
        REQUIRE (std::abs (sound[i] - sound[i - 1]) < 1.1f * steepest);
}

TEST_CASE ("Unison voices added while a note sounds play from the synth's next control step, wherever a block ends")
{
    // 441 samples a block at 44.1 kHz, as Windows' shared mode hands them: a block ends 9 samples into a control step of 16.
    // Unison Voices raised there, the rest of that step still plays the voices the step set up, and the new ones start with
    // the next (they were read at once, from readers no step had set: no table, and a crash).
    constexpr int blockSize = 441;

    for (const auto warp : { Synth::Warp::none, Synth::Warp::fm })
    {
        INFO ("warp " << (int) warp);
        Synth synth;
        synth.prepare (44100.0, blockSize);
        auto values = getPlainValues (0.0f);
        values[Synth::osc2On] = 1.0f;
        values[Synth::osc1Warp] = (float) warp;
        values[Synth::osc1WarpAmount] = 0.25f;
        values[Synth::unisonDetune] = 20.0f;
        auto sound = play (synth, values, blockSize, { noteOn (0, 57) }, blockSize);

        for (const auto voices : { 8, 1, 5, 2, 8 })
        {
            values[Synth::unisonVoices] = (float) (voices - 1);
            const auto more = play (synth, values, 3 * blockSize, {}, blockSize);
            sound.left.insert (sound.left.end(), more.left.begin(), more.left.end());
            sound.right.insert (sound.right.end(), more.right.begin(), more.right.end());
        }

        // The note goes on, on both sides, at about the level it had: two oscillators at full level, however many voices.
        for (const auto* side : { &sound.left, &sound.right })
        {
            CHECK (std::all_of (side->begin(), side->end(), [] (float sample) { return std::isfinite (sample); }));
            CHECK (peakOf (*side, 0, side->size()) < 3.0f);

            for (size_t from = blockSize; from + blockSize <= side->size(); from += blockSize)
                CHECK (peakOf (*side, from, from + blockSize) > 0.2f);
        }
    }
}

TEST_CASE ("The synth's filter darkens its saw, and the default patch opens it with each note")
{
    const auto tenthOf = [] (const std::vector<float>& values)
    {
        const auto played = play (*makeSynth(), values, 14400, { noteOn (0, 45) });
        return measureLevelAt (played.left, 4800, 14400, 10.0 * noteFrequency (45))
               / measureLevelAt (played.left, 4800, 14400, noteFrequency (45));
    };

    auto values = getPlainValues (2.0f / 3.0f);
    const auto open = tenthOf (values);
    values[Synth::filterOn] = 1.0f;
    values[Synth::filterType] = (float) devices::dsp::FilterMode::lowPass;
    values[Synth::cutoff] = 400.0f;
    values[Synth::keyTracking] = 0.0f;
    CHECK (tenthOf (values) < 0.05 * open); // about 35 dB down, an octave and a half above the cutoff

    // A new synth: a saw through its filter, which its second envelope opens two octaves wider at each note's start, so its
    // 30th harmonic comes through at first and then fades.
    auto synth = makeSynth();
    const auto played = play (*synth, getDefaults(), 24000, { noteOn (0, 45) });
    const auto brightness = [&played] (size_t from, size_t to)
    {
        return measureLevelAt (played.left, from, to, 30.0 * noteFrequency (45))
               / measureLevelAt (played.left, from, to, noteFrequency (45));
    };
    CHECK (brightness (0, 2400) > 2.0 * brightness (19200, 24000));
}

TEST_CASE ("The synth's combs: on a note's pitch, Comb + keeps a saw's harmonics and Comb - leaves out its even ones")
{
    // The types every Phazite filter offers (dsp/MultimodeFilter.h), in its order.
    const auto& types = Synth::info.parameters[Synth::filterType].choices;
    REQUIRE (types.size() == 9);
    CHECK (types[(size_t) devices::dsp::FilterMode::notch] == std::string_view ("Notch"));
    CHECK (types[(size_t) devices::dsp::FilterMode::combPlus] == std::string_view ("Comb +"));
    CHECK (types[(size_t) devices::dsp::FilterMode::combMinus] == std::string_view ("Comb -"));

    // Basic Shapes' saw at A3 (220 Hz) through a comb tuned to it: its first four harmonics.
    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::cutoff] = 220.0f;
    values[Synth::resonance] = 0.5f;
    values[Synth::keyTracking] = 0.0f;
    const auto harmonicsThrough = [&values] (devices::dsp::FilterMode type)
    {
        values[Synth::filterType] = (float) type;
        auto synth = makeSynth();
        const auto played = play (*synth, values, 24000, { noteOn (0, 57) });
        std::array<double, 4> levels {};

        for (size_t h = 0; h < levels.size(); ++h)
            levels[h] = measureLevelAt (played.left, 4800, 24000, 220.0 * (double) (h + 1));

        return levels;
    };

    const auto plus = harmonicsThrough (devices::dsp::FilterMode::combPlus);
    CHECK (plus[1] / plus[0] == doctest::Approx (1.0 / 2.0).epsilon (0.1));
    CHECK (plus[2] / plus[0] == doctest::Approx (1.0 / 3.0).epsilon (0.1));
    CHECK (plus[3] / plus[0] == doctest::Approx (1.0 / 4.0).epsilon (0.1));

    const auto minus = harmonicsThrough (devices::dsp::FilterMode::combMinus);
    CHECK (minus[2] / minus[0] == doctest::Approx (1.0 / 3.0).epsilon (0.1));
    CHECK (minus[1] < 0.01 * minus[0]);
    CHECK (minus[3] < 0.01 * minus[0]);

    // Swept by an envelope for each note, it stays steady.
    values[Synth::mod1Source] = (float) Synth::ModSource::envelope2;
    values[Synth::mod1Target] = (float) Synth::ModTarget::cutoff;
    values[Synth::mod1Amount] = 1.0f;
    values[Synth::resonance] = 1.0f;
    const auto swept = play (*makeSynth(), values, 24000, { noteOn (0, 45), noteOn (2400, 52), noteOn (4800, 57) });
    auto peak = 0.0f;

    for (const auto sample : swept.left)
        peak = std::max (peak, std::abs (sample));

    CHECK (std::isfinite (peak));
    CHECK (peak > 0.05f);
    CHECK (peak < 4.0f);
}

TEST_CASE ("The synth's vowel, phaser and sample and hold play its saw, steadily")
{
    const auto& types = Synth::info.parameters[Synth::filterType].choices;
    CHECK (types[(size_t) devices::dsp::FilterMode::vowel] == std::string_view ("Vowel"));
    CHECK (types[(size_t) devices::dsp::FilterMode::phaser] == std::string_view ("Phaser"));
    CHECK (types[(size_t) devices::dsp::FilterMode::sampleAndHold] == std::string_view ("Sample & Hold"));

    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::cutoff] = 1000.0f;
    values[Synth::resonance] = 1.0f;

    for (const auto type : { devices::dsp::FilterMode::vowel, devices::dsp::FilterMode::phaser, devices::dsp::FilterMode::sampleAndHold })
    {
        INFO ("type " << (int) type);
        values[Synth::filterType] = (float) type;
        const auto played = play (*makeSynth(), values, 24000, { noteOn (0, 45), noteOn (2400, 52) });
        auto peak = 0.0f;

        for (const auto sample : played.left)
            peak = std::max (peak, std::abs (sample));

        CHECK (std::isfinite (peak));
        CHECK (peak > 0.05f);
        CHECK (peak < 4.0f);
    }

    // Sample and hold at a cutoff of 1 kHz folds a saw's 25th harmonic (2750 Hz at A2) back to 750 Hz, which a plain saw
    // at A2 hasn't (its harmonics are at multiples of 110 Hz).
    values[Synth::filterType] = (float) devices::dsp::FilterMode::sampleAndHold;
    values[Synth::resonance] = 0.0f;
    const auto held = play (*makeSynth(), values, 24000, { noteOn (0, 45) });
    CHECK (measureLevelAt (held.left, 4800, 24000, 750.0) > 0.002);
    values[Synth::filterOn] = 0.0f;
    const auto plain = play (*makeSynth(), values, 24000, { noteOn (0, 45) });
    CHECK (measureLevelAt (plain.left, 4800, 24000, 750.0) < 0.0005);
}

TEST_CASE ("The synth plays 16 voices, glides in mono mode, and plays the same every time")
{
    auto synth = makeSynth();
    auto values = getDefaults();
    std::vector<NoteEvent> chord;

    for (int note = 48; note < 68; ++note)
        chord.push_back (noteOn (0, note));

    play (*synth, values, 4800, chord);
    CHECK (synth->getActiveVoices() == Synth::maxVoices);

    // Mono: one voice, which glides to the next key held without starting again.
    values = getPlainValues (0.0f);
    values[Synth::voiceMode] = 1.0f;
    values[Synth::glide] = 20.0f;
    auto mono = makeSynth();
    const auto glided = play (*mono, values, 48000, { noteOn (0, 57), noteOn (24000, 69) });
    CHECK (mono->getActiveVoices() == 1);
    CHECK (measureFrequency (glided.left, 12000, 24000) == doctest::Approx (220.0).epsilon (0.003));
    CHECK (measureFrequency (glided.left, 38400, 48000) == doctest::Approx (440.0).epsilon (0.003));

    // With unison, noise, a random LFO on the position and random values for each note: the same twice.
    values = getDefaults();
    values[Synth::unisonVoices] = 6.0f;
    values[Synth::noiseLevel] = 0.3f;
    values[Synth::lfo1Shape] = 5.0f; // random
    values[Synth::mod2Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod2Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod2Amount] = 0.5f;
    values[Synth::mod3Source] = (float) Synth::ModSource::random;
    values[Synth::mod3Target] = (float) Synth::ModTarget::pan;
    values[Synth::mod3Amount] = 1.0f;
    const std::vector<NoteEvent> notes { noteOn (0, 48), noteOn (3000, 55), noteOff (9000, 48), noteOn (12000, 60) };
    auto first = makeSynth();
    auto second = makeSynth();
    const auto once = play (*first, values, 24000, notes);
    const auto again = play (*second, values, 24000, notes);
    CHECK (once.left == again.left);
    CHECK (once.right == again.right);
}

TEST_CASE ("A key played again on the synth as it rings out takes a voice of its own: its tail rings on, the new note starts "
           "afresh")
{
    // A new synth's saw through its filter, which its second envelope opens for each note: C3 let go, then played
    // again a third of the way through its 200 ms release. It plays each note as it does alone, added, on both sides: the
    // first's tail as it would have rung out, the second from the start of its wave and of its envelopes, as a note on a
    // voice of its own. Its voice taken back, the second would have gone on with the first's wave and envelopes.
    const auto [played, added] =
        playApart (getDefaults(), 36000, { noteOn (0, 48), noteOff (4800, 48), noteOn (8000, 48), noteOff (19200, 48) },
                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (8000, 48), noteOff (19200, 48) } });
    REQUIRE (peakOf (played.left, 0, 4800) > 0.1f);
    CHECK (largestDifference (played, added) < 1.0e-6f);
    CHECK (played.voicesAfter[40] == 2); // 0.22 s: the first's tail with the second
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the tail has rung out, at 0.3 s
    CHECK (played.voicesAfter.back() == 0);
}

TEST_CASE ("A note-off lets go of its key's held voice only on the synth: a tail of the same key rings on, its release not "
           "begun again")
{
    // C3 let go, a note-off while only its tail sounds, which lets go of nothing, then C3 again, let go while the first's tail
    // still rings: that note-off lets go of the second note only. Begun again, the first's release would have rung on past
    // the 200 ms it takes.
    const auto [played, added] =
        playApart (getDefaults(), 36000, { noteOn (0, 48), noteOff (4800, 48), noteOff (6400, 48), noteOn (8000, 48), noteOff (9600, 48) },
                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (8000, 48), noteOff (9600, 48) } });
    CHECK (largestDifference (played, added) < 1.0e-6f);
    CHECK (played.voicesAfter[40] == 2); // the two tails
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the first's rang out at 0.3 s, the second's rings until 0.4 s
    CHECK (played.voicesAfter[76] == 0);
}

TEST_CASE ("A key played again on the synth while it is held lets its voice go as if it came up; the new note takes a voice "
           "of its own")
{
    // Two notes of C3 that overlap: the first lets go as the second begins, its release ringing on, and the first note-off
    // lets go of the key, which then holds nothing for the second. A key never holds two voices.
    const auto [played, added] =
        playApart (getDefaults(), 36000, { noteOn (0, 48), noteOn (3200, 48), noteOff (4800, 48), noteOff (8000, 48) },
                   { { noteOn (0, 48), noteOff (3200, 48) }, { noteOn (3200, 48), noteOff (4800, 48) } });
    CHECK (largestDifference (played, added) < 1.0e-6f);
    CHECK (played.voicesAfter[14] == 2); // 0.08 s: the first's tail, and the second held
}

TEST_CASE ("Under the sustain pedal, a key played again on the synth lets the voice the pedal holds ring out, and the pedal "
           "holds the new one")
{
    // The pedal down, C3 played and let go: the pedal holds it. Played again, the voice the pedal held lets go, its release
    // ringing on, and the new note takes a voice of its own, which the pedal holds until it lifts: one voice held for the key.
    const auto [played, added] = playApart (
        getDefaults(), 36000,
        { pedal (0, true), noteOn (0, 48), noteOff (1600, 48), noteOn (4800, 48), noteOff (6400, 48), pedal (9600, false) },
        { { noteOn (0, 48), noteOff (4800, 48) }, { pedal (0, true), noteOn (4800, 48), noteOff (6400, 48), pedal (9600, false) } });
    CHECK (largestDifference (played, added) < 1.0e-6f);
    CHECK (played.voicesAfter[30] == 2); // 0.17 s: the first's tail, and the second that the pedal holds
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the first's tail rang out at 0.3 s; the pedal let go of the second at 0.2 s
    CHECK (played.voicesAfter[76] == 0);
}

TEST_CASE ("In mono mode a key played again on the synth as it rings out takes the one voice back from where it is, as before")
{
    // The table's sine, released over 200 ms and played again a third of the way through: one voice still, its wave going on
    // where it was and its level rising from where its release had brought it, without a step (a sine at 131 Hz moves at
    // most 0.02 a sample, its attack's rise with it; started again, the wave would have jumped by its level, 0.2 here).
    auto values = getPlainValues (0.0f);
    values[Synth::voiceMode] = 1.0f;
    const auto played = play (*makeSynth(), values, 36000, { noteOn (0, 48), noteOff (4800, 48), noteOn (8000, 48), noteOff (19200, 48) });
    CHECK (*std::max_element (played.voicesAfter.begin(), played.voicesAfter.end()) == 1);
    REQUIRE (peakOf (played.left, 7600, 8000) > 0.1f); // its tail, as it is played again
    CHECK (largestStep (played.left, 7900, 8300) < 0.05f);
}

TEST_CASE ("The synth's modulation keeps its time, however blocks and notes split the samples")
{
    // LFO 1 moves the volume and an envelope the table position: a note held, and another that comes and goes, played in
    // the usual blocks and in blocks of 37 samples, sound the same.
    auto values = getPlainValues (0.0f);
    values[Synth::lfo1Rate] = 7.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod1Target] = (float) Synth::ModTarget::volume;
    values[Synth::mod1Amount] = 1.0f;
    values[Synth::mod2Source] = (float) Synth::ModSource::envelope2;
    values[Synth::mod2Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod2Amount] = 1.0f;
    const std::vector<NoteEvent> events { noteOn (0, 60), noteOn (1000, 67), noteOff (5003, 67) };

    auto inBlocks = makeSynth();
    auto inPieces = makeSynth();
    const auto steady = play (*inBlocks, values, 24000, events);
    const auto split = play (*inPieces, values, 24000, events, 37);

    auto largest = 0.0f, loudest = 0.0f;

    for (size_t i = 0; i < steady.left.size(); ++i)
    {
        largest = std::max (largest, std::abs (steady.left[i] - split.left[i]));
        loudest = std::max (loudest, std::abs (steady.left[i]));
    }

    CHECK (loudest > 0.1f);
    CHECK (largest < 1.0e-4f);
}

TEST_CASE ("Stopping playback cuts the synth's notes that had only just begun, and lets the others ring out")
{
    // C3 from the start, and C5 begun 5 ms before all notes off at 0.5 s: from 20 ms to 100 ms after it, how much is left
    // of each, played on the first table's sine.
    auto values = getPlainValues (0.0f);
    values[Synth::env1Release] = 200.0f;
    constexpr int offAt = 24064; // a block's start
    const std::vector<NoteEvent> events { noteOn (0, 48), noteOn (offAt - 240, 72), { NoteEvent::Type::allNotesOff, offAt, 0, 0.0f } };

    // Stopped: C5 is gone, while C3 rings out over its release.
    auto stoppedSynth = makeSynth();
    const auto stopped = play (*stoppedSynth, values, 48000, events, testBlockSize, offAt);
    CHECK (measureLevelAt (stopped.left, offAt + 960, offAt + 4800, noteFrequency (72)) < 0.001);
    CHECK (measureLevelAt (stopped.left, offAt + 960, offAt + 4800, noteFrequency (48)) > 0.05);

    // A jump while playing lets both ring out.
    auto jumpedSynth = makeSynth();
    const auto jumped = play (*jumpedSynth, values, 48000, events, testBlockSize, 48000);
    CHECK (measureLevelAt (jumped.left, offAt + 960, offAt + 4800, noteFrequency (72)) > 0.05);
    CHECK (measureLevelAt (jumped.left, offAt + 960, offAt + 4800, noteFrequency (48)) > 0.05);
}

TEST_CASE ("The synth lets go of a table it no longer plays once the audio thread can't be reading it")
{
    auto synth = makeSynth();
    auto table = Wavetable::fromHarmonics ("Mine", { makeSaw() });
    const std::weak_ptr<const Wavetable> mine = table;
    synth->setTable (0, std::move (table));
    CHECK (synth->getTable (0) == mine.lock());

    // Swapped for a factory table before the synth played a step on it: never taken by the audio thread, it goes at once.
    synth->setTable (0, devices::getFactoryWavetable (1));
    CHECK (mine.expired());

    const auto values = getDefaults();
    play (*synth, values, 2 * testBlockSize, { noteOn (0, 60) });
    synth->setTable (0, devices::getFactoryWavetable (2));
    CHECK (mine.expired());

    // Nothing given: the first factory table.
    synth->setTable (1, nullptr);
    CHECK (synth->getTable (1) == devices::getFactoryWavetable (0));
}

TEST_CASE ("A table knows what a part of a frame's cycle adds up to, and the mean of the frame read with its places bent")
{
    // A saw rising from -1 to 1 over its cycle, whose area from its start to x is x^2 - x; and its mean read bent, against the
    // mean of its samples read at the bent places themselves, many to a cycle.
    const auto table = Wavetable::fromHarmonics ("Saw", { makeSaw() });
    const auto cycle = table->getFrameForDrawing (0);
    const auto length = (int) cycle.size();
    const auto scale = table->getArea (0, 0, 0.5f) / -0.25f; // the made saw's height and way, its peak at 1
    REQUIRE (std::abs (scale) > 0.5f);

    for (const auto x : { 0.1f, 0.25f, 0.77f, 0.9f })
        CHECK (table->getArea (0, 0, x) == doctest::Approx (scale * (x * x - x)).epsilon (0.02));

    CHECK (std::abs (table->getArea (0, 0, 1.0f)) < 1.0e-4f);

    for (const auto bend : { 0.125f, 0.3f, 0.7f, 1.0f, 1.6f, 3.0f, 8.0f })
    {
        CAPTURE (bend);
        constexpr int points = 1 << 18;
        auto sum = 0.0;

        for (int n = 0; n < points; ++n)
        {
            const auto t = ((double) n + 0.5) / points;
            const auto place = bend * t / (1.0 + (bend - 1.0) * t);
            sum += cycle[(size_t) std::min (length - 1, (int) (place * length))];
        }

        CHECK (table->getBentMean (0, 0, Wavetable::findBend (bend)) == doctest::Approx (sum / points).epsilon (0.003).scale (1.0));
    }
}

TEST_CASE ("The synth's warps that read a part of the wave, or read it unevenly, leave nothing off centre")
{
    // Sync, Bend and Pulse Width read a part of a frame's cycle, read it unevenly, or hold its start, which is no longer
    // centred: each takes what it leaves off centre away as it plays. What lies under a held note, as a part of how loud the
    // note is (DspProbe.h's getOffsetShare(), through a Hann window): under four thousandths, near where it is (the device
    // conformance suite asks for under a fiftieth; the reviewer's finding 1, 2026-11-15, found 1.6 % left at high bends).
    // Nothing after the corrections takes anything away: the DC blocker plays only with Ring Mod or FM (the second
    // review's finding 2, 2026-11-15). Without the corrections, the worst leaves a quarter of the note's level.
    const auto offsetShare = [] (const std::vector<float>& sound, size_t from, size_t to)
    {
        auto sum = 0.0, squares = 0.0, weights = 0.0;

        for (auto i = from; i < to; ++i)
        {
            const auto weight = 0.5 - 0.5 * std::cos (twoPi * (double) (i - from) / (double) (to - from));
            sum += weight * sound[i];
            squares += weight * sound[i] * sound[i];
            weights += weight;
        }

        return std::abs (sum / weights) / std::sqrt (squares / weights);
    };

    auto worst = 0.0;

    for (const auto warp : { Synth::Warp::sync, Synth::Warp::bendUp, Synth::Warp::bendDown, Synth::Warp::pulseWidth })
    {
        for (const auto position : { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 0.9f })
        {
            for (const auto amount : { 0.15f, 0.35f, 0.55f, 0.77f, 0.95f })
            {
                auto values = getPlainValues (position);
                values[Synth::osc1Warp] = (float) warp;
                values[Synth::osc1WarpAmount] = amount;

                for (const auto note : { 57, 84 }) // the higher read from a level with fewer harmonics, whose area differs
                {
                    const auto played = play (*makeSynth(), values, 19200, { noteOn (0, note) });
                    const auto share = offsetShare (played.left, 4800, 19200);
                    INFO ("warp " << (int) warp << " at " << amount << ", position " << position << ", note " << note << ": " << share);
                    CHECK (share < 0.004);
                    worst = std::max (worst, share);
                }
            }
        }
    }

    MESSAGE ("the most left off centre under a warped note: " << worst << " of its level");
}

TEST_CASE ("A table swapped out is never let go of while a control step still reads it, however short the blocks")
{
    // Blocks of one sample, as the runner's stretches between control points can be: a table swapped out in the middle of a
    // control step is still read until the step ends, which is calls later. The test keeps its own hold on it, and once the
    // synth has let go of it fills it with what isn't a number: any read of it after that comes out in what the synth plays.
    auto synth = makeSynth();
    auto first = Wavetable::fromHarmonics ("Saw", { makeSaw() });
    synth->setTable (0, first);
    const auto values = getDefaults();
    play (*synth, values, 1024 + 5, { noteOn (0, 60) }); // 5 samples into a control step

    const auto poison = [&first]
    {
        for (int frame = 0; frame < first->getNumFrames(); ++frame)
        {
            for (int level = 0; level < Wavetable::numLevels; ++level)
            {
                const auto read = first->getLevel (frame, level);
                auto* samples = const_cast<float*> (read.samples);

                for (int i = -1; i < read.length + 2; ++i)
                    samples[i] = std::numeric_limits<float>::quiet_NaN();
            }
        }
    };

    auto poisoned = false, allNumbers = true;

    for (int i = 0; i < 64; ++i)
    {
        // Another table each sample, which also lets go of what the synth can.
        synth->setTable (0, devices::getFactoryWavetable (1 + i % 2));

        if (! poisoned && first.use_count() == 1)
        {
            poison();
            poisoned = true;
        }

        const auto played = play (*synth, values, 1, {}, 1);
        allNumbers = allNumbers && std::isfinite (played.left[0]) && std::isfinite (played.right[0]);
    }

    CHECK (poisoned); // let go of, once its step was over
    CHECK (allNumbers);
}

TEST_CASE ("A table's bent mean is the mean of its level as the synth reads it, at a level of few harmonics too, saw and square")
{
    // The reviewer's finding 1 (2026-11-15).
    // What a reader plays bent, from the level's own samples by cubic interpolation (readCubic(), as the synth reads them),
    // many to a cycle, against the mean worked out from the frame's harmonics: within a five-hundredth of the wave's peak, at
    // the level with all of them, one in the middle and one with 16.
    Wavetable::Harmonics square ((size_t) Wavetable::maxHarmonics);

    for (int h = 1; h <= Wavetable::maxHarmonics; h += 2)
        square[(size_t) h - 1] = { 0.0f, -1.0f / (float) h };

    const auto table = Wavetable::fromHarmonics ("Saw and square", { makeSaw(), square });
    auto worst = 0.0;

    for (int frame = 0; frame < 2; ++frame)
    {
        for (const auto level : { 0, 6, 12 })
        {
            const auto read = table->getLevel (frame, level);

            for (const auto bend : { 0.125f, 0.2f, 0.45f, 1.7f, 3.3f, 6.4f, 8.0f })
            {
                CAPTURE (frame);
                CAPTURE (level);
                CAPTURE (bend);
                constexpr int points = 1 << 17;
                auto sum = 0.0;

                for (int n = 0; n < points; ++n)
                {
                    const auto t = ((double) n + 0.5) / points;
                    const auto place = bend * t / (1.0 + (bend - 1.0) * t);
                    sum += devices::readCubic (read, (float) (place * read.length));
                }

                const auto off = std::abs (table->getBentMean (frame, level, Wavetable::findBend (bend)) - sum / points);
                CHECK (off < 0.002);
                worst = std::max (worst, off);
            }
        }
    }

    MESSAGE ("the most a bent mean is off what the synth reads: " << worst);
}

TEST_CASE ("A synth that plays nothing holds at most two of the tables given it, and lets go of one it played by itself")
{
    // The reviewer's finding 3 (2026-11-15).
    // A hundred tables given while the synth isn't processed (a muted track's): each but the newest goes as the next comes,
    // since the audio thread never took it. Counted by what of them is still alive.
    auto synth = makeSynth();
    std::vector<std::weak_ptr<const Wavetable>> given;

    for (int n = 0; n < 100; ++n)
    {
        auto table = Wavetable::fromHarmonics ("Saw", { makeSaw() });
        given.push_back (table);
        synth->setTable (0, std::move (table));
    }

    const auto alive = std::count_if (given.begin(), given.end(), [] (const auto& each) { return ! each.expired(); });
    CHECK (alive <= 2);
    CHECK (alive >= 1); // the newest, which it plays from its next step

    // One it played, swapped for another: gone once the synth has played on a step, when its owner next asks (the playback
    // session's timer), without another being given.
    const auto values = getDefaults();
    play (*synth, values, 64, { noteOn (0, 60) });
    const std::weak_ptr<const Wavetable> played = synth->getTable (0);
    synth->setTable (0, Wavetable::fromHarmonics ("Saw", { makeSaw() }));
    CHECK (! played.expired()); // a step under way may read it
    play (*synth, values, 64, {});
    synth->releaseUnused();
    CHECK (played.expired());
}

TEST_CASE ("The synth's Ring Mod and FM warps leave nothing off centre at whole ratios")
{
    // The reviewer's finding 6 (2026-11-15).
    // Osc 2 at osc 1's pitch, or an octave up: ring modulation of two waves at one pitch has a part at 0 Hz, as FM's sidebands
    // at whole ratios do. What the DC blocker on the synth's sum takes away: under the conformance suite's fiftieth.
    const auto offsetShare = [] (const std::vector<float>& sound, size_t from, size_t to)
    {
        auto sum = 0.0, squares = 0.0, weights = 0.0;

        for (auto i = from; i < to; ++i)
        {
            const auto weight = 0.5 - 0.5 * std::cos (twoPi * (double) (i - from) / (double) (to - from));
            sum += weight * sound[i];
            squares += weight * sound[i] * sound[i];
            weights += weight;
        }

        return std::abs (sum / weights) / std::sqrt (squares / weights);
    };

    auto worst = 0.0;

    for (const auto warp : { Synth::Warp::ringMod, Synth::Warp::fm })
    {
        for (const auto pitch : { 0.0f, 12.0f })
        {
            for (const auto amount : { 0.5f, 1.0f })
            {
                auto values = getPlainValues (0.0f); // the sine
                values[Synth::osc1Warp] = (float) warp;
                values[Synth::osc1WarpAmount] = amount;
                values[Synth::osc2On] = 1.0f;
                values[Synth::osc2Pitch] = pitch;
                values[Synth::osc2Position] = 0.0f;
                const auto played = play (*makeSynth(), values, 24000, { noteOn (0, 57) });
                const auto share = offsetShare (played.left, 12000, 24000);
                INFO ("warp " << (int) warp << ", osc 2 " << pitch << " semitones up, amount " << amount << ": " << share);
                CHECK (share < 0.02);
                worst = std::max (worst, share);
            }
        }
    }

    MESSAGE ("the most left off centre under ring modulation and FM at whole ratios: " << worst);
}

TEST_CASE ("The synth's warps take what they leave off centre away smoothly as their amount and position move")
{
    // The reviewer's finding 8 (2026-11-15).
    // An envelope sweeping Osc 1's warp amount and its position, for Sync, Bend and Pulse Width: what is taken away moves at
    // each control step, so a step in it would show as larger jumps between samples at the steps' ends than between
    // others. The mean jump at the steps' ends against the mean of the rest: alike.
    for (const auto warp : { Synth::Warp::sync, Synth::Warp::bendUp, Synth::Warp::pulseWidth })
    {
        auto values = getPlainValues (2.0f / 3.0f);
        values[Synth::osc1Warp] = (float) warp;
        values[Synth::osc1WarpAmount] = 0.2f;
        values[Synth::mod1Source] = (float) Synth::ModSource::envelope2;
        values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Warp;
        values[Synth::mod1Amount] = 0.7f;
        values[Synth::mod2Source] = (float) Synth::ModSource::envelope2;
        values[Synth::mod2Target] = (float) Synth::ModTarget::osc1Position;
        values[Synth::mod2Amount] = -0.3f;
        values[Synth::env2Decay] = 400.0f;
        // From where its latency ends, so that the steps' ends are every 16th sample from the start.
        const auto latency = Synth::getLatency();
        auto played = play (*makeSynth(), values, 19200 + latency, { noteOn (0, 40) });
        played.left.erase (played.left.begin(), played.left.begin() + latency);
        auto atEnds = 0.0, elsewhere = 0.0;
        auto numEnds = 0, numElsewhere = 0;

        for (size_t i = 1000; i < 19200; ++i)
        {
            const auto jump = std::abs ((double) played.left[i] - played.left[i - 1]);
            (i % 16 == 0 ? atEnds : elsewhere) += jump;
            ++(i % 16 == 0 ? numEnds : numElsewhere);
        }

        INFO ("warp " << (int) warp);
        CHECK ((atEnds / numEnds) < 1.1 * (elsewhere / numElsewhere));
    }
}

namespace
{
// Plays the synth in blocks of a size, the song playing, each block at the song's sample songSampleOf (block), with events at
// their samples in the first block.
std::vector<float> playAt (Synth& synth, const std::vector<float>& values, int blockSize, int numBlocks,
                           const std::function<std::int64_t (int)>& songSampleOf, const std::vector<NoteEvent>& events)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> played;

    for (int b = 0; b < numBlocks; ++b)
    {
        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = blockSize;
        block.parameters = values;
        block.events = b == 0 ? std::span<const NoteEvent> (events) : std::span<const NoteEvent>();
        block.isPlaying = true;
        block.songSample = songSampleOf (b);
        synth.process (block);

        for (int i = 0; i < blockSize; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}
} // namespace

TEST_CASE ("A clean synth patch plays as without a DC blocker: its note's area left in it, and nothing after it")
{
    // The second review's finding 1 (2026-11-15): the blocker on the synth's sum changed every patch's waveform, a clean
    // one's too (it took out a note's net area and put it back as a slow tail). Now it plays only while an oscillator has
    // the Ring Mod or FM warp. A plain wave and a short note: once it has gone, every sample is exactly 0, which a blocker
    // that had heard it would not leave; and with Ring Mod, where it plays, its tail goes on.
    const auto lastSounding = [] (bool ringModulated)
    {
        auto values = getPlainValues (0.3f);
        values[Synth::env1Release] = 30.0f;

        if (ringModulated)
        {
            values[Synth::osc2On] = 1.0f;
            values[Synth::osc1Warp] = (float) Synth::Warp::ringMod;
            values[Synth::osc1WarpAmount] = 1.0f;
        }

        const auto played = play (*makeSynth(), values, (int) testRate, { noteOn (0, 40), noteOff ((int) (0.1 * testRate), 40) });
        auto last = 0;

        for (int i = 0; i < (int) played.left.size(); ++i)
            if (! juce::exactlyEqual (played.left[(size_t) i], 0.0f))
                last = i;

        return last;
    };

    CHECK (lastSounding (false) < (int) (0.3 * testRate));
    CHECK (lastSounding (true) > (int) (0.9 * testRate));
}

TEST_CASE ("A table's area across a part of its cycle is that of its level as the synth reads it, at a level of few harmonics too")
{
    // The second review's finding 3 (2026-11-15): the areas were the trapezoids between a level's samples, which the synth
    // reads by cubic interpolation; now each is worked out exactly from the harmonics the level keeps, as the bent means
    // are. At the places they are kept at, against the integral of what readCubic() plays, many points to a cycle: saw and
    // square, at the level with all the harmonics, one in the middle and one with 16.
    Wavetable::Harmonics square ((size_t) Wavetable::maxHarmonics);

    for (int h = 1; h <= Wavetable::maxHarmonics; h += 2)
        square[(size_t) h - 1] = { 0.0f, -1.0f / (float) h };

    const auto table = Wavetable::fromHarmonics ("Saw and square", { makeSaw(), square });
    auto worst = 0.0;

    for (int frame = 0; frame < 2; ++frame)
    {
        for (const auto level : { 0, 6, 12 })
        {
            CAPTURE (frame);
            CAPTURE (level);
            const auto read = table->getLevel (frame, level);
            constexpr int points = 1 << 17, perPlace = points / Wavetable::areaPoints;
            auto sum = 0.0;

            for (int n = 0; n < points; ++n)
            {
                sum += devices::readCubic (read, (float) (((double) n + 0.5) / points * read.length));

                if ((n + 1) % perPlace == 0)
                {
                    const auto place = (n + 1) / perPlace;
                    const auto off = std::abs (table->getArea (frame, level, (float) place / Wavetable::areaPoints) - sum / points);
                    CHECK (off < 4.0e-4);
                    worst = std::max (worst, off);
                }
            }
        }
    }

    MESSAGE ("the most an area is off what the synth reads: " << worst);
}

TEST_CASE ("The synth's envelopes and LFOs keep their pace where a loop goes round, its last step cut short")
{
    // The second review's finding 5 (2026-11-15): a step the grid cut short moved its control-rate parts a whole step on. A
    // loop of 161 samples (ten steps and one sample) goes round 300 times: each pass's last step, cut after one sample, moved
    // them 16 samples on, 4500 over the passes. Against a play straight on through as many samples: where the free LFO 2 has
    // put Osc 1's position, and Envelope 3, rising over 3 s, Osc 2's.
    auto values = getPlainValues (0.25f);
    values[Synth::osc2On] = 1.0f;
    values[Synth::osc2Position] = 0.0f;
    values[Synth::lfo2Retrigger] = 0.0f;
    values[Synth::lfo2Rate] = 0.5f;
    values[Synth::mod2Source] = (float) Synth::ModSource::lfo2;
    values[Synth::mod2Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod2Amount] = 0.5f;
    values[Synth::env3Attack] = 3000.0f;
    values[Synth::env3Sustain] = 1.0f;
    values[Synth::mod3Source] = (float) Synth::ModSource::envelope3;
    values[Synth::mod3Target] = (float) Synth::ModTarget::osc2Position;
    values[Synth::mod3Amount] = 0.8f;
    constexpr int loopLength = 161, passes = 300;

    const auto positions = [&values] (bool looped)
    {
        auto synth = makeSynth();
        playAt (*synth, values, loopLength, passes,
                [looped] (int b) { return looped ? std::int64_t { 0 } : (std::int64_t) b * loopLength; }, { noteOn (0, 57) });
        return std::pair { synth->getPlayedPosition (0), synth->getPlayedPosition (1) };
    };

    const auto straight = positions (false), looped = positions (true);
    INFO ("straight " << straight.first << ", " << straight.second << "; looped " << looped.first << ", " << looped.second);
    REQUIRE (std::abs (straight.first - 0.25f) > 0.05f); // the LFO has moved it
    CHECK (std::abs (looped.first - straight.first) < 0.002f);
    CHECK (std::abs (looped.second - straight.second) < 0.002f);
}

TEST_CASE ("The synth's comb glides its delay across a step the grid cut to one sample, rather than jumping to it")
{
    // The second review's finding 5 (2026-11-15). Comb +, its cutoff swept by Envelope 2, a sine through it; in blocks of 17
    // samples, each where the song has jumped to a step's last sample, so that every block begins with a step of one sample,
    // after which the delay jumped to its target. How much what plays bends from sample to sample (its second difference)
    // at those samples, against elsewhere.
    auto values = getPlainValues (0.0f); // a sine
    values[Synth::filterOn] = 1.0f;
    values[Synth::filterType] = (float) devices::dsp::FilterMode::combPlus;
    values[Synth::cutoff] = 400.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::envelope2;
    values[Synth::mod1Target] = (float) Synth::ModTarget::cutoff;
    values[Synth::mod1Amount] = -0.5f;
    values[Synth::env2Decay] = 1000.0f;
    values[Synth::env2Sustain] = 0.0f;
    constexpr int blockSize = 17, numBlocks = 1200;
    const auto played =
        playAt (*makeSynth(), values, blockSize, numBlocks, [] (int b) { return (std::int64_t) (32 * b + 15); }, { noteOn (0, 57) });
    auto atCut = 0.0, elsewhere = 0.0;
    auto numAtCut = 0, numElsewhere = 0;

    for (size_t i = 4 * blockSize; i < played.size(); ++i)
    {
        const auto bent = std::abs ((double) played[i] - 2.0 * played[i - 1] + played[i - 2]);
        const auto cut = i % blockSize <= 1;
        (cut ? atCut : elsewhere) += bent;
        ++(cut ? numAtCut : numElsewhere);
    }

    INFO ("at the cut steps " << atCut / numAtCut << ", elsewhere " << elsewhere / numElsewhere);
    CHECK (atCut / numAtCut < 1.5 * (elsewhere / numElsewhere));
}

//==============================================================================
// What the synth took from the synth (roadmap step S6.7): each LFO's fade, Glide Mode, the Drift source, slots that
// move other slots' amounts, and Sync's reach to 16 times.
namespace
{
NoteEvent wheel (int sample, float value)
{
    return { NoteEvent::Type::controller, sample, 1, value };
}

// Plays the synth as play() does, calling `afterBlock` after each block.
void playWatching (Synth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
                   const std::function<void()>& afterBlock)
{
    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        std::vector<NoteEvent> inBlock;

        for (auto event : events)
        {
            if (event.sample >= done && event.sample < done + testBlockSize)
            {
                event.sample -= done;
                inBlock.push_back (event);
            }
        }

        play (synth, values, testBlockSize, inBlock);
        afterBlock();
    }
}

// How far a steady tone's pitch swings between two samples, in semitones: from the lowest to the highest frequency its
// cycles take, each from one rising zero crossing to the next (each crossing placed between its samples).
double measurePitchSwing (const std::vector<float>& sound, size_t from, size_t to)
{
    auto lowest = std::numeric_limits<double>::max(), highest = 0.0, last = -1.0;

    for (auto i = from + 1; i < to && i < sound.size(); ++i)
    {
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
        {
            const auto crossing = (double) i - 1.0 + sound[i - 1] / ((double) sound[i - 1] - sound[i]);

            if (last >= 0.0)
            {
                const auto frequency = testRate / (crossing - last);
                lowest = std::min (lowest, frequency);
                highest = std::max (highest, frequency);
            }

            last = crossing;
        }
    }

    return highest > 0.0 ? 12.0 * std::log2 (highest / lowest) : 0.0;
}
} // namespace

TEST_CASE ("The synth's LFO fade brings each note's share of the LFO in from nothing, from that note's start")
{
    // A sine at A4 and LFO 1 on its pitch, a semitone either way at 5 Hz, faded in over 400 ms: the swing grows from nothing
    // to two semitones, for a note let go and played again too, whether the LFO starts with each note or runs on for all.
    for (const auto retriggered : { true, false })
    {
        CAPTURE (retriggered);
        auto values = getPlainValues (0.0f);
        values[Synth::env1Release] = 0.5f; // the first note gone before the second
        values[Synth::lfo1Rate] = 5.0f;
        values[Synth::lfo1Retrigger] = retriggered ? 1.0f : 0.0f;
        values[Synth::lfo1FadeIn] = 400.0f;
        values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
        values[Synth::mod1Target] = (float) Synth::ModTarget::pitch;
        values[Synth::mod1Amount] = 1.0f / 24.0f;
        const auto played = play (*makeSynth(), values, 86400, { noteOn (0, 69), noteOff (33600, 69), noteOn (38400, 69) });

        const auto early = measurePitchSwing (played.left, 0, 4800), later = measurePitchSwing (played.left, 9600, 14400);
        INFO ("the first 100 ms " << early << ", from 200 to 300 ms " << later);
        CHECK (early < 0.3);
        CHECK (later > early + 0.3);
        CHECK (later < 1.8);
        CHECK (measurePitchSwing (played.left, 19200, 33600) == doctest::Approx (2.0).epsilon (0.05)); // whole from 400 ms

        // The second note's fade starts with it.
        CHECK (measurePitchSwing (played.left, 38400 + 240, 38400 + 4800) < 0.3);
        CHECK (measurePitchSwing (played.left, 38400 + 19200, 86400) == doctest::Approx (2.0).epsilon (0.05));
    }

    // In mono mode a legato note goes on with its voice's fade: the swing is whole at once, an octave up.
    auto values = getPlainValues (0.0f);
    values[Synth::voiceMode] = 1.0f;
    values[Synth::lfo1Rate] = 5.0f;
    values[Synth::lfo1FadeIn] = 400.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod1Target] = (float) Synth::ModTarget::pitch;
    values[Synth::mod1Amount] = 1.0f / 24.0f;
    const auto legato = play (*makeSynth(), values, 48000, { noteOn (0, 69), noteOn (28800, 81) });
    CHECK (measureFrequency (legato.left, 28800 + 480, 48000) == doctest::Approx (880.0).epsilon (0.03));
    CHECK (measurePitchSwing (legato.left, 28800 + 480, 48000) == doctest::Approx (2.0).epsilon (0.05));

    // With no fade, the LFO is there from the note's start: a semitone up within its first 100 ms.
    values[Synth::voiceMode] = 0.0f;
    values[Synth::lfo1FadeIn] = 0.0f;
    CHECK (measurePitchSwing (play (*makeSynth(), values, 4800, { noteOn (0, 69) }).left, 0, 4800) > 0.9);
}

TEST_CASE ("In Legato glide mode only a note played while another is held glides, in poly and mono mode")
{
    // The Note source on Osc 1's position: where the newest note plays in the table follows its pitch, C4 at the table's
    // start, C5 a quarter of the way along. Just after C5 starts, a glide from C4 has barely begun; without one it is there.
    auto values = getPlainValues (0.0f);
    values[Synth::glide] = 100.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::note;
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod1Amount] = 1.0f;

    const auto positionAfter = [&values] (int samples, bool held)
    {
        auto synth = makeSynth();
        play (*synth, values, samples,
              held ? std::vector<NoteEvent> { noteOn (0, 60), noteOn (12000, 72) }
                   : std::vector<NoteEvent> { noteOn (0, 60), noteOff (12000, 60), noteOn (12000, 72) });
        return synth->getPlayedPosition (0);
    };

    for (const auto mono : { false, true })
    {
        CAPTURE (mono);
        values[Synth::voiceMode] = mono ? 1.0f : 0.0f;
        values[Synth::glideMode] = (float) Synth::GlideMode::legato;

        // C4 let go, then C5: no glide, C5 at once.
        CHECK (positionAfter (12032, false) == doctest::Approx (0.25f));

        // C4 held, then C5: it glides there.
        CHECK (positionAfter (12032, true) < 0.05f);
        CHECK (positionAfter (48000, true) == doctest::Approx (0.25f).epsilon (0.01));

        // Always, as before: the note let go of still glides into the next.
        values[Synth::glideMode] = (float) Synth::GlideMode::always;
        CHECK (positionAfter (12032, false) < 0.05f);
    }
}

TEST_CASE ("The synth's Drift source lets each voice's cutoff wander on its own, within what its slot's amount allows")
{
    // Drift on the cutoff at 2 semitones (an analog voice's drift at full), the keys not followed: the newest note's cutoff
    // wanders around 1 kHz, no further than 2 semitones either way; a second voice wanders a way of its own; and two renders
    // are the same.
    auto values = getPlainValues (0.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::cutoff] = 1000.0f;
    values[Synth::keyTracking] = 0.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::drift;
    values[Synth::mod1Target] = (float) Synth::ModTarget::cutoff;
    values[Synth::mod1Amount] = 2.0f / 96.0f;

    const auto cutoffs = [&values] (const std::vector<NoteEvent>& events)
    {
        auto synth = makeSynth();
        std::vector<float> shown;
        playWatching (*synth, values, 96000, events, [&] { shown.push_back (synth->getShown().cutoff.get()); });
        return shown;
    };

    const auto first = cutoffs ({ noteOn (0, 60) });
    const auto second = cutoffs ({ noteOn (0, 60), noteOn (0, 64) }); // the newest, 64, on the second voice
    CHECK (cutoffs ({ noteOn (0, 60), noteOn (0, 64) }) == second);

    const auto lowest = 1000.0f * std::exp2 (-2.0f / 12.0f) * 0.999f, highest = 1000.0f * std::exp2 (2.0f / 12.0f) * 1.001f;

    for (const auto* each : { &first, &second })
    {
        const auto [low, high] = std::minmax_element (each->begin(), each->end());
        INFO ("from " << *low << " to " << *high << " Hz");
        CHECK (*low >= lowest);
        CHECK (*high <= highest);
        CHECK (12.0f * std::log2 (*high / *low) > 0.5f); // it wanders
    }

    auto apart = 0.0f;

    for (size_t i = 0; i < first.size(); ++i)
        apart = std::max (apart, std::abs (12.0f * std::log2 (first[i] / second[i])));

    CHECK (apart > 0.5f);
}

TEST_CASE ("The synth's Drift source takes nothing from the other random values: unused, everything plays as it did")
{
    // A patch with unison drift, noise and a random value for each note: with a slot that plays Drift at nothing, and with
    // its source on nothing, every sample the same.
    auto values = getDefaults();
    values[Synth::unisonVoices] = 3.0f;
    values[Synth::unisonDrift] = 20.0f;
    values[Synth::noiseLevel] = 0.2f;
    values[Synth::mod2Source] = (float) Synth::ModSource::random;
    values[Synth::mod2Target] = (float) Synth::ModTarget::pan;
    values[Synth::mod2Amount] = 0.5f;
    const std::vector<NoteEvent> notes { noteOn (0, 48), noteOn (3000, 55), noteOff (9000, 48), noteOn (12000, 60) };
    const auto without = play (*makeSynth(), values, 24000, notes);

    values[Synth::mod3Source] = (float) Synth::ModSource::drift;
    values[Synth::mod3Target] = (float) Synth::ModTarget::cutoff;
    values[Synth::mod3Amount] = 0.0f;
    const auto with = play (*makeSynth(), values, 24000, notes);
    CHECK (with.left == without.left);
    CHECK (with.right == without.right);
}

TEST_CASE ("A slot of the synth can move another slot's amount: the mod wheel brings an LFO's vibrato in from nothing")
{
    // Slot 2: LFO 1 on the pitch at nothing. Slot 1: the mod wheel on Mod 2 Amount at 50 %. Without the wheel, a steady A4;
    // the wheel up, slot 2 at half its range: twelve semitones either way.
    auto values = getPlainValues (0.0f);
    values[Synth::lfo1Rate] = 2.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::modWheel;
    values[Synth::mod1Target] = (float) Synth::ModTarget::slot2Amount;
    values[Synth::mod1Amount] = 0.5f;
    values[Synth::mod2Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod2Target] = (float) Synth::ModTarget::pitch;
    values[Synth::mod2Amount] = 0.0f;

    const auto still = play (*makeSynth(), values, 24000, { noteOn (0, 69) });
    CHECK (measurePitchSwing (still.left, 480, 24000) < 0.01);
    CHECK (measureFrequency (still.left, 480, 24000) == doctest::Approx (440.0).epsilon (0.002));

    const auto moving = play (*makeSynth(), values, 24000, { wheel (0, 1.0f), noteOn (0, 69) });
    CHECK (measurePitchSwing (moving.left, 480, 24000) == doctest::Approx (24.0).epsilon (0.05));

    // The amount moved stays in its range: slot 2 at 80 % and the wheel adding 50 % plays it at 100 %, 24 semitones up while
    // a square LFO is high, not 31.
    values[Synth::lfo1Shape] = (float) devices::dsp::LfoShape::square;
    values[Synth::lfo1Rate] = 1.0f;
    values[Synth::mod2Amount] = 0.8f;
    const auto kept = play (*makeSynth(), values, 19200, { wheel (0, 1.0f), noteOn (0, 45) });
    CHECK (measureFrequency (kept.left, 480, 19200) == doctest::Approx (440.0).epsilon (0.002));
}

TEST_CASE ("A slot of the synth on its own amount, and two on each other's, change nothing and don't loop")
{
    // LFO 1 on Osc 1's position, and the other slots on amounts: on their own, or each on the other's. A slot that aims at an
    // amount plays by its own as set, which nothing moves, so the sound is that of the first slot alone.
    auto values = getPlainValues (0.3f);
    values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod1Amount] = 0.4f;
    const std::vector<NoteEvent> notes { wheel (0, 1.0f), noteOn (0, 57), noteOn (4000, 64) };
    const auto alone = play (*makeSynth(), values, 19200, notes);

    auto own = values;
    own[Synth::mod2Source] = (float) Synth::ModSource::modWheel;
    own[Synth::mod2Target] = (float) Synth::ModTarget::slot2Amount;
    own[Synth::mod2Amount] = 1.0f;
    CHECK (play (*makeSynth(), own, 19200, notes).left == alone.left);

    auto each = values;
    each[Synth::mod2Source] = (float) Synth::ModSource::lfo2;
    each[Synth::mod2Target] = (float) Synth::ModTarget::slot3Amount;
    each[Synth::mod2Amount] = 1.0f;
    each[Synth::mod3Source] = (float) Synth::ModSource::modWheel;
    each[Synth::mod3Target] = (float) Synth::ModTarget::slot2Amount;
    each[Synth::mod3Amount] = -1.0f;
    CHECK (play (*makeSynth(), each, 19200, notes).left == alone.left);
}

TEST_CASE ("The synth's slots that move amounts don't depend on their order: a moved amount moves nothing further")
{
    // Slot 1: LFO 1 on Osc 1's position at 25 %. Slot 3: the mod wheel on Mod 1 Amount at 25 %. Slot 2: the mod wheel on Mod 3
    // Amount at 50 %. A slot aimed at an amount plays by its own amount as set, so slot 3 adds 25 % to slot 1 whatever slot 2
    // does to it: the sound of slot 1 alone at 50 %. Were the amounts moved in the slots' order, slot 3 would add 75 %.
    auto values = getPlainValues (0.3f);
    values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod1Amount] = 0.5f;
    const std::vector<NoteEvent> notes { wheel (0, 1.0f), noteOn (0, 57), noteOn (4000, 64) };
    const auto alone = play (*makeSynth(), values, 19200, notes);

    const auto chain = [&] (int moverOfSlot3, int moverOfSlot1)
    {
        auto chained = values;
        chained[Synth::mod1Amount] = 0.25f;
        chained[(size_t) Synth::getModParameter (moverOfSlot1, Synth::modSource)] = (float) Synth::ModSource::modWheel;
        chained[(size_t) Synth::getModParameter (moverOfSlot1, Synth::modTarget)] = (float) Synth::ModTarget::slot1Amount;
        chained[(size_t) Synth::getModParameter (moverOfSlot1, Synth::modAmount)] = 0.25f;
        chained[(size_t) Synth::getModParameter (moverOfSlot3, Synth::modSource)] = (float) Synth::ModSource::modWheel;
        chained[(size_t) Synth::getModParameter (moverOfSlot3, Synth::modTarget)] =
            (float) (moverOfSlot1 == 2 ? Synth::ModTarget::slot3Amount : Synth::ModTarget::slot2Amount);
        chained[(size_t) Synth::getModParameter (moverOfSlot3, Synth::modAmount)] = 0.5f;
        return play (*makeSynth(), chained, 19200, notes);
    };

    // The slot that moves slot 1's amount before the slot that moves its own, and after it.
    CHECK (chain (1, 2).left == alone.left);
    CHECK (chain (2, 1).left == alone.left);
}

TEST_CASE ("The synth's Sync warp reaches 16 times the note's cycle by modulation, the synth's sweep of four octaves")
{
    // A sine synced: at the knob's end it reads its wave 8 times a cycle, its 8th harmonic loudest; a slot from the mod wheel
    // to the warp at a third more takes it to 16 times, its 16th.
    auto values = getPlainValues (0.0f);
    values[Synth::osc1Warp] = (float) Synth::Warp::sync;
    values[Synth::osc1WarpAmount] = 1.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::modWheel;
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Warp;
    values[Synth::mod1Amount] = 1.0f / 3.0f;
    const auto f0 = noteFrequency (45);

    const auto knob = play (*makeSynth(), values, 19200, { noteOn (0, 45) });
    CHECK (measureLevelAt (knob.left, 4800, 19200, 8.0 * f0) > 10.0 * measureLevelAt (knob.left, 4800, 19200, 16.0 * f0));

    const auto swept = play (*makeSynth(), values, 19200, { wheel (0, 1.0f), noteOn (0, 45) });
    CHECK (measureLevelAt (swept.left, 4800, 19200, 16.0 * f0) > 10.0 * measureLevelAt (swept.left, 4800, 19200, 8.0 * f0));
}

TEST_CASE ("The synth's LFO fade, legato glide, Drift and a slot on another's amount play alike in blocks of 32 and 512")
{
    // Every one of them in use while notes overlap, the wheel moving in the middle: what plays doesn't depend on how the
    // samples come in blocks.
    auto values = getPlainValues (0.2f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::glide] = 80.0f;
    values[Synth::glideMode] = (float) Synth::GlideMode::legato;
    values[Synth::lfo1FadeIn] = 150.0f;
    values[Synth::lfo2FadeIn] = 90.0f;
    values[Synth::lfo2Retrigger] = 0.0f;
    values[Synth::lfo2Rate] = 3.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Position;
    values[Synth::mod1Amount] = 0.0f;
    values[Synth::mod2Source] = (float) Synth::ModSource::modWheel;
    values[Synth::mod2Target] = (float) Synth::ModTarget::slot1Amount;
    values[Synth::mod2Amount] = 0.6f;
    values[Synth::mod3Source] = (float) Synth::ModSource::drift;
    values[Synth::mod3Target] = (float) Synth::ModTarget::cutoff;
    values[Synth::mod3Amount] = 0.1f;
    values[Synth::mod4Source] = (float) Synth::ModSource::lfo2;
    values[Synth::mod4Target] = (float) Synth::ModTarget::pitch;
    values[Synth::mod4Amount] = 0.05f;
    const std::vector<NoteEvent> events { noteOn (0, 60), noteOn (1000, 67), wheel (3000, 0.7f), noteOff (5003, 67), noteOn (9001, 64) };

    const auto small = play (*makeSynth(), values, 24000, events, 32);
    const auto large = play (*makeSynth(), values, 24000, events, 512);
    CHECK (peakOf (small.left, 0, 24000) > 0.05f);
    CHECK (largestDifference (small, large) < 1.0e-4f);
}

TEST_CASE ("The synth's resonance makes a 12 dB section's Q from 0.5 to 25")
{
    // The classic presets want the filter's whole reach: a 12 dB
    // low-pass at no resonance rounds its cutoff off, a Q of 0.5 (6 dB down there, where a Butterworth's 0.71 is 3 dB down),
    // and at full it rings at a Q of 25 (the Filter device's curve stops at 16).
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (parameter.defaultValue);

    values[Synth::filterSlope] = 0.0f; // 12 dB
    constexpr double rate = 48000.0, cutoff = 1000.0;
    const auto atCutoff = [&] (float resonance)
    {
        values[Synth::resonance] = resonance;
        return juce::Decibels::gainToDecibels (Synth::getFilterMagnitude (values, cutoff, cutoff, rate));
    };

    CHECK (atCutoff (0.0f) == doctest::Approx (juce::Decibels::gainToDecibels (0.5)).epsilon (0.02));
    CHECK (atCutoff (1.0f) == doctest::Approx (juce::Decibels::gainToDecibels (25.0)).epsilon (0.02));

    // And dsp::getSynthResonanceQ()'s curve between.
    for (const auto resonance : { 0.1f, 0.35f, 0.6f, 0.9f })
    {
        CAPTURE (resonance);
        CHECK (atCutoff (resonance)
               == doctest::Approx (juce::Decibels::gainToDecibels (devices::dsp::getSynthResonanceQ (resonance))).epsilon (0.02));
    }
}

TEST_CASE ("Basic Shapes' frames are the sub's own waves, in phase and polarity, and the Pulse Width table's frames its pulses")
{
    // So that two oscillators an octave apart, or one and the sub, sum as an analog synth's do (a saw under a pulse, as Duo
    // Lead and DCO Bass play them): Basic Shapes' sine, triangle, saw and square, and each of the Pulse Width table's pulses
    // (from 50 % down to 2 %), against the sub's band-limited oscillator reading its own waves (Oscillators.h),
    // each with all its harmonics: the same shape, not turned over nor moved along its cycle.
    devices::dsp::Oscillator oscillator;
    oscillator.setFrequency (20.0, testRate); // every harmonic

    const auto compare = [&oscillator] (const Wavetable& table, int frame, devices::dsp::Waveform wave, float width)
    {
        const auto level = table.getLevel (frame, 0);
        auto both = 0.0, tables = 0.0, synths = 0.0;

        for (int i = 0; i < level.length; ++i)
        {
            const auto a = (double) level.samples[i];
            const auto b = (double) oscillator.valueAt (wave, (float) i / (float) level.length, width);
            both += a * b;
            tables += a * a;
            synths += b * b;
        }

        return both / std::sqrt (tables * synths);
    };

    const auto basic = devices::getFactoryWavetable (devices::findFactoryWavetable ("Basic Shapes"));
    REQUIRE (basic->getNumFrames() == 4);

    for (const auto wave :
         { devices::dsp::Waveform::sine, devices::dsp::Waveform::triangle, devices::dsp::Waveform::saw, devices::dsp::Waveform::square })
    {
        CAPTURE ((int) wave);
        CHECK (compare (*basic, (int) wave, wave, 0.5f) > 0.999);
    }

    const auto pulses = devices::getFactoryWavetable (devices::findFactoryWavetable ("Pulse Width"));

    for (int frame = 0; frame < pulses->getNumFrames(); ++frame)
    {
        CAPTURE (frame);
        const auto width = 0.5f - 0.48f * (float) frame / (float) (pulses->getNumFrames() - 1);
        if (width >= 0.05f) // the synth's narrowest
            CHECK (compare (*pulses, frame, devices::dsp::Waveform::square, width) > 0.999);
    }
}

TEST_CASE ("The synth's Phase: Reset starts each note from its waves' starts, Free lets a voice's waves run on")
{
    // One voice played twice (the second note on the voice the first left), its first oscillator with the sub: on Reset, as
    // before, both notes start alike; on Free, the second starts where the voice's waves were as the first ended. Free is the
    // same every time, and in blocks of any size, in unison too.
    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::subLevel] = 0.5f;
    values[Synth::env1Release] = 5.0f;
    const auto noteLength = (int) testRate / 4, secondAt = (int) testRate / 2, compared = 2048;
    const std::vector<NoteEvent> events { noteOn (0, 48), noteOff (noteLength, 48), noteOn (secondAt, 48),
                                          noteOff (secondAt + noteLength, 48) };

    const auto playWith = [&] (Synth::Phase phase, int blockSize)
    {
        auto synth = makeSynth();
        auto with = values;
        with[Synth::phase] = (float) phase;
        return play (*synth, with, secondAt + noteLength + 4800, events, blockSize);
    };

    const auto startsAlike = [compared, secondAt] (const Played& played)
    {
        for (int i = 0; i < compared; ++i)
            if (std::abs (played.left[(size_t) i] - played.left[(size_t) (secondAt + i)]) > 1.0e-6f)
                return false;

        return true;
    };

    const auto reset = playWith (Synth::Phase::reset, testBlockSize);
    CHECK (startsAlike (reset));
    CHECK (Synth::info.parameters[Synth::phase].defaultValue == doctest::Approx ((float) Synth::Phase::reset));

    const auto free = playWith (Synth::Phase::free, testBlockSize);
    CHECK_FALSE (startsAlike (free));

    // Its first note starts as Reset's does: a voice that has never played starts from its waves' starts.
    for (int i = 0; i < noteLength; ++i)
        REQUIRE (free.left[(size_t) i] == doctest::Approx (reset.left[(size_t) i]).epsilon (1.0e-6));

    values[Synth::unisonVoices] = 2.0f; // three
    const auto inUnison = playWith (Synth::Phase::free, testBlockSize);

    for (const auto blockSize : { 32, 512 })
    {
        CAPTURE (blockSize);
        const auto again = playWith (Synth::Phase::free, blockSize);
        auto largest = 0.0f;

        for (size_t i = 0; i < inUnison.left.size(); ++i)
            largest = std::max ({ largest, std::abs (again.left[i] - inUnison.left[i]), std::abs (again.right[i] - inUnison.right[i]) });

        CHECK (largest < 1.0e-5f);
    }

    values[Synth::unisonVoices] = 0.0f;

    // In Mono too: the one voice runs on.
    values[Synth::voiceMode] = 1.0f;
    CHECK (startsAlike (playWith (Synth::Phase::reset, testBlockSize)));
    CHECK_FALSE (startsAlike (playWith (Synth::Phase::free, testBlockSize)));
}

TEST_CASE ("The synth's Drift wanders on its own for each slot: on each oscillator's pitch, they beat against each other")
{
    // Two sines on one note, each on its own oscillator, at one level. Drift on each one's pitch, from two slots (as an analog
    // voice's two oscillators drift, each on its own): their pitches wander apart, and their sum beats, its level swinging far.
    // The same Drift on both together (one slot on Pitch) moves them as one: their sum holds its level. Two renders agree, and
    // so do blocks of 32 and 512 samples.
    auto values = getPlainValues (0.0f);
    values[Synth::osc2On] = 1.0f;
    values[Synth::osc2Position] = 0.0f;
    values[Synth::osc2Level] = 1.0f;
    values[Synth::level] = -6.0f;
    constexpr int numSamples = 96000, frame = 480;

    const auto swing = [] (const Played& played)
    {
        auto low = 0.0f, high = 0.0f;

        for (int start = 4800; start + frame <= numSamples; start += frame)
        {
            auto energy = 0.0f;

            for (int i = start; i < start + frame; ++i)
                energy += played.left[(size_t) i] * played.left[(size_t) i];

            const auto level = juce::Decibels::gainToDecibels (std::sqrt (energy / (float) frame), -100.0f);
            low = start == 4800 ? level : std::min (low, level);
            high = start == 4800 ? level : std::max (high, level);
        }

        return high - low;
    };

    // Together: one slot on both oscillators' pitch.
    values[Synth::mod1Source] = (float) Synth::ModSource::drift;
    values[Synth::mod1Target] = (float) Synth::ModTarget::pitch;
    values[Synth::mod1Amount] = 1.0f / 24.0f;
    const auto together = play (*makeSynth(), values, numSamples, { noteOn (0, 69) });
    CHECK (swing (together) < 0.5f);

    // Apart: a slot on each.
    values[Synth::mod1Target] = (float) Synth::ModTarget::osc1Pitch;
    values[Synth::mod2Source] = (float) Synth::ModSource::drift;
    values[Synth::mod2Target] = (float) Synth::ModTarget::osc2Pitch;
    values[Synth::mod2Amount] = 1.0f / 24.0f;
    const auto apart = play (*makeSynth(), values, numSamples, { noteOn (0, 69) });
    INFO ("together " << swing (together) << " dB, apart " << swing (apart) << " dB");
    CHECK (swing (apart) > 6.0f);
    CHECK (play (*makeSynth(), values, numSamples, { noteOn (0, 69) }).left == apart.left);

    for (const auto blockSize : { 32, 512 })
    {
        CAPTURE (blockSize);
        const auto again = play (*makeSynth(), values, numSamples, { noteOn (0, 69) }, blockSize);
        auto largest = 0.0f;

        for (size_t i = 0; i < apart.left.size(); ++i)
            largest = std::max (largest, std::abs (again.left[i] - apart.left[i]));

        CHECK (largest < 1.0e-5f);
    }
}

TEST_CASE ("A synth note plays as loud as its velocity says, counted squared, as keyboards feel")
{
    // A sine with velocity fully counted: half velocity is a quarter of the level; with none counted, any velocity plays at full.
    auto values = getPlainValues (0.0f);
    values[Synth::velocity] = 1.0f;
    const auto full = peakOf (play (*makeSynth(), values, 9600, { noteOn (0, 69) }).left, 4800, 9600);
    REQUIRE (full > 0.1f);
    CHECK (peakOf (play (*makeSynth(), values, 9600, { noteOn (0, 69, 0.5f) }).left, 4800, 9600) / full
           == doctest::Approx (0.25f).epsilon (0.01));

    values[Synth::velocity] = 0.0f;
    CHECK (peakOf (play (*makeSynth(), values, 9600, { noteOn (0, 69, 0.5f) }).left, 4800, 9600) == doctest::Approx (full).epsilon (0.01));
}

TEST_CASE ("The synth plays chords, rings notes out over their release, and steals the oldest voice when it runs out")
{
    auto values = getPlainValues (0.0f);
    values[Synth::env1Release] = 100.0f;
    const auto alone = measureLevelAt (play (*makeSynth(), values, 24000, { noteOn (0, 60) }).left, 2400, 9600, noteFrequency (60));
    REQUIRE (alone > 0.1);

    // Each note of a C major chord as loud as alone, and nothing between them.
    const auto chord =
        play (*makeSynth(), values, 24000,
              { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOff (9600, 60), noteOff (9600, 64), noteOff (9600, 67) });

    for (const auto note : { 60, 64, 67 })
        CHECK (measureLevelAt (chord.left, 2400, 9600, noteFrequency (note)) == doctest::Approx (alone).epsilon (0.05));

    CHECK (measureLevelAt (chord.left, 2400, 9600, 300.0) < 0.05 * alone);
    CHECK (chord.voicesAfter[10] == 3);
    CHECK (chord.voicesAfter.back() == 0); // the 100 ms release is over

    // Seventeen keys at once: sixteen voices, the first note taken for the last. Almost a second, to tell semitones apart.
    std::vector<NoteEvent> keys;

    for (int note = 60; note < 77; ++note)
        keys.push_back (noteOn (note - 60, note));

    auto crowded = makeSynth();
    const auto all = play (*crowded, values, 48000, keys);
    CHECK (crowded->getActiveVoices() == Synth::maxVoices);
    CHECK (measureLevelAt (all.left, 4800, 48000, noteFrequency (60)) < 0.05 * alone);
    CHECK (measureLevelAt (all.left, 4800, 48000, noteFrequency (61)) > 0.5 * alone);
    CHECK (measureLevelAt (all.left, 4800, 48000, noteFrequency (76)) > 0.5 * alone);
}

TEST_CASE ("A note that takes a sounding voice fades it out, and starts from silence")
{
    // All 16 voices hold a note; a 17th takes the oldest. With a 50 ms attack, the new note rises from silence.
    auto values = getPlainValues (0.0f);
    values[Synth::env1Attack] = 50.0f;
    std::vector<NoteEvent> events;

    for (int n = 0; n < Synth::maxVoices; ++n)
        events.push_back (noteOn (0, 40 + n));

    events.push_back (noteOn (4800, 80));
    const auto played = play (*makeSynth(), values, 14400, events);

    // Just after the 17th note, the voice it took fades out besides the 16; 3 ms later it is gone.
    CHECK (played.voicesAfter[18] == Synth::maxVoices + 1); // the block from 4608 to 4864
    CHECK (played.voicesAfter[19] == Synth::maxVoices);

    // The new note (830.6 Hz) starts quietly, then reaches its level.
    const auto rising = measureLevelAt (played.left, 4810, 5050, 830.6);
    const auto risen = measureLevelAt (played.left, 9600, 14400, 830.6);
    CHECK (risen > 0.1);
    CHECK (rising < 0.3 * risen);
}

TEST_CASE ("The synth bends its pitch, and glides from key to key in mono mode without starting again")
{
    auto values = getPlainValues (0.0f);

    // A full bend up of a 2-semitone range.
    auto played = play (*makeSynth(), values, 24000, { { NoteEvent::Type::pitchBend, 0, 0, 1.0f }, noteOn (0, 69) });
    CHECK (measureFrequency (played.left, 4800, 24000) == doctest::Approx (440.0 * std::pow (2.0, 2.0 / 12.0)).epsilon (0.001));

    // Mono, a 50 ms glide: C4 held, then C5 while it is still held, so the voice glides and doesn't start again.
    values[Synth::voiceMode] = 1.0f;
    values[Synth::glide] = 50.0f;
    auto mono = makeSynth();
    played = play (*mono, values, 48000, { noteOn (0, 60), noteOn (24000, 72) });
    CHECK (mono->getActiveVoices() == 1);
    CHECK (measureFrequency (played.left, 4800, 24000) == doctest::Approx (261.63).epsilon (0.002));
    CHECK (measureFrequency (played.left, 36000, 48000) == doctest::Approx (523.25).epsilon (0.002));
    CHECK (peakOf (played.left, 24000, 24480) > 0.95f * peakOf (played.left, 12000, 24000)); // no new attack: the level stays up

    // Letting go of C5 goes back to C4, which is still held.
    played = play (*makeSynth(), values, 48000, { noteOn (0, 60), noteOn (4800, 72), noteOff (9600, 72) });
    CHECK (measureFrequency (played.left, 24000, 48000) == doctest::Approx (261.63).epsilon (0.002));
}

TEST_CASE ("The synth's glide keeps its time, however blocks and notes split the samples")
{
    // Mono, a 100 ms glide from C4 up to C5: played in the usual blocks, and in blocks of 37 samples.
    auto values = getPlainValues (0.0f);
    values[Synth::voiceMode] = 1.0f;
    values[Synth::glide] = 100.0f;
    const std::vector<NoteEvent> events { noteOn (0, 60), noteOn (4800, 72) };
    const auto steady = play (*makeSynth(), values, 14400, events);
    const auto split = play (*makeSynth(), values, 14400, events, 37);

    // Halfway through the glide, both are at the same pitch.
    const auto expected = measureFrequency (steady.left, 7000, 8000);
    CHECK (expected > 300.0);
    CHECK (expected < 500.0);
    CHECK (measureFrequency (split.left, 7000, 8000) == doctest::Approx (expected).epsilon (0.002));
}

TEST_CASE ("The synth's sustain pedal holds notes until it lifts, and all notes off lets everything go")
{
    auto values = getPlainValues (0.0f);
    values[Synth::env1Release] = 50.0f;
    auto synth = makeSynth();
    const auto held = play (*synth, values, 36000, { pedal (0, true), noteOn (0, 60), noteOff (4800, 60), pedal (24000, false) });
    CHECK (held.voicesAfter[80] == 1); // 0.43 s: the key is up, the pedal down
    CHECK (held.voicesAfter.back() == 0);

    play (*synth, values, 4800, { pedal (0, true), noteOn (0, 48), noteOn (0, 55), noteOff (2400, 48) });
    REQUIRE (synth->getActiveVoices() == 2);
    play (*synth, values, 24000, { { NoteEvent::Type::allNotesOff, 0, 0, 0.0f } }); // and the release passes
    CHECK (synth->getActiveVoices() == 0);
}

TEST_CASE ("A slide note glides the sounding notes to its pitch evenly over its length, and a portamento note glides in")
{
    // C4, with a slide to C5 a quarter of a second in, half a second long: half way there half way through, there after.
    const auto values = getPlainValues (0.0f);
    auto slid = makeSynth();
    auto played = play (*slid, values, 72000, { noteOn (0, 60), { NoteEvent::Type::slide, 12000, 72, 0.5f } });
    CHECK (measureFrequency (played.left, 2400, 11000) == doctest::Approx (261.63).epsilon (0.002));
    CHECK (measureFrequency (played.left, 23000, 25000) == doctest::Approx (261.63 * std::sqrt (2.0)).epsilon (0.03));
    CHECK (measureFrequency (played.left, 40000, 72000) == doctest::Approx (523.25).epsilon (0.002));
    CHECK (slid->getActiveVoices() == 1); // the slide played no note of its own

    // A portamento note after C4 starts there and glides up to its own pitch in a tenth of a second (Glide is off). In mono
    // mode, so that C4's release doesn't sound with it.
    auto mono = values;
    mono[Synth::voiceMode] = 1.0f;
    played = play (*makeSynth(), mono, 72000, { noteOn (0, 60), noteOff (24000, 60), { NoteEvent::Type::portamentoOn, 24000, 72, 1.0f } });
    CHECK (measureFrequency (played.left, 24000, 25200) < 380.0);
    CHECK (measureFrequency (played.left, 36000, 72000) == doctest::Approx (523.25).epsilon (0.002));

    // A plain note after C4 starts on its own pitch.
    played = play (*makeSynth(), mono, 48000, { noteOn (0, 60), noteOff (24000, 60), noteOn (24000, 72) });
    CHECK (measureFrequency (played.left, 24480, 30000) == doctest::Approx (523.25).epsilon (0.002));
}

TEST_CASE (
    "Envelope 3 on the pitch drops each note onto its pitch over its decay, as an 808 drops, and a legato note doesn't start it again")
{
    // An octave up at the start (12 of the slot's 24 semitones), falling over 300 ms, fast at first: near A5 in the first few
    // milliseconds, A4 once it is down.
    auto values = getPlainValues (0.0f);
    values[Synth::env3Attack] = 0.0f;
    values[Synth::env3Decay] = 300.0f;
    values[Synth::env3Sustain] = 0.0f;
    values[Synth::env3DecayCurve] = -1.0f;
    values[Synth::mod2Source] = (float) Synth::ModSource::envelope3;
    values[Synth::mod2Target] = (float) Synth::ModTarget::pitch;
    values[Synth::mod2Amount] = 0.5f;
    const auto played = play (*makeSynth(), values, 24000, { noteOn (0, 69) });
    CHECK (measureFrequency (played.left, 0, 240) > 800.0);
    CHECK (measureFrequency (played.left, 14400, 24000) == doctest::Approx (440.0).epsilon (0.002));

    // A legato note in mono mode plays at its own pitch: the envelope, down, doesn't start again.
    values[Synth::voiceMode] = 1.0f;
    const auto legato = play (*makeSynth(), values, 28800, { noteOn (0, 69), noteOn (14400, 57) });
    CHECK (measureFrequency (legato.left, 14640, 28800) == doctest::Approx (220.0).epsilon (0.002));
}

TEST_CASE ("The synth's sub plays an octave or two down, as a sine, triangle, saw or square")
{
    auto values = getPlainValues (0.0f);
    values[Synth::osc1Level] = 0.0f;
    values[Synth::subLevel] = 1.0f;
    const auto subAt = [&values] (int octave, int shape, double frequency)
    {
        auto set = values;
        set[Synth::subOctave] = (float) octave;
        set[Synth::subShape] = (float) shape;
        return measureLevelAt (play (*makeSynth(), set, 48000, { noteOn (0, 69) }).left, 4800, 48000, frequency);
    };

    const auto sine = subAt (0, 0, 220.0); // a sine an octave down
    REQUIRE (sine > 0.1);
    CHECK (subAt (1, 0, 110.0) == doctest::Approx (sine).epsilon (0.05)); // two octaves down
    CHECK (subAt (1, 0, 220.0) < 0.01 * sine);                            // and not one
    CHECK (subAt (0, 3, 660.0) > 0.3 * sine);                             // a square: its third harmonic
    CHECK (subAt (0, 0, 660.0) < 0.01 * sine);                            // which a sine hasn't
}

TEST_CASE ("A Velocity slot on the cutoff closes the synth's filter for softer notes")
{
    // A saw through a 24 dB low-pass at 500 Hz that velocity opens by up to two octaves: at full velocity at 2 kHz, at half at
    // 1 kHz, so its harmonics around 2 kHz are much weaker.
    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::cutoff] = 500.0f;
    values[Synth::keyTracking] = 0.0f;
    values[Synth::mod2Source] = (float) Synth::ModSource::velocity;
    values[Synth::mod2Target] = (float) Synth::ModTarget::cutoff;
    values[Synth::mod2Amount] = 0.25f;
    const auto bright = [&values] (float velocity)
    {
        const auto played = play (*makeSynth(), values, 24000, { noteOn (0, 45, velocity) });
        return measureLevelAt (played.left, 4800, 24000, 1980.0) / measureLevelAt (played.left, 4800, 24000, 110.0);
    };

    CHECK (bright (0.5f) < 0.2 * bright (1.0f));

    // At no amount, velocity doesn't move the cutoff.
    values[Synth::mod2Amount] = 0.0f;
    CHECK (bright (0.5f) == doctest::Approx (bright (1.0f)).epsilon (0.01));
}

TEST_CASE ("The synth's notch takes out a sine at its frequency, and its 24 dB slope falls further than its 12 dB")
{
    const auto& types = Synth::info.parameters[Synth::filterType].choices;
    CHECK (types[(size_t) devices::dsp::FilterMode::notch] == std::string_view ("Notch"));

    // A saw at A2 (110 Hz) through a low-pass at 500 Hz: at 24 dB, its 27th harmonic falls much further than at 12.
    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::cutoff] = 500.0f;
    values[Synth::resonance] = 0.0f;
    values[Synth::keyTracking] = 0.0f;
    const auto trebleAt = [&values] (float slope)
    {
        values[Synth::filterSlope] = slope;
        return measureLevelAt (play (*makeSynth(), values, 24000, { noteOn (0, 45) }).left, 4800, 24000, 2970.0);
    };

    CHECK (trebleAt (1.0f) < 0.1 * trebleAt (0.0f));

    // The notch takes out a sine at its frequency (A4, 440 Hz), against the open low-pass.
    values = getPlainValues (0.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::keyTracking] = 0.0f;
    const auto rmsThrough = [&values] (devices::dsp::FilterMode type, float frequency)
    {
        values[Synth::filterType] = (float) type;
        values[Synth::cutoff] = frequency;
        const auto played = play (*makeSynth(), values, 24000, { noteOn (0, 69) }).left;
        auto sum = 0.0;

        for (size_t i = 12000; i < played.size(); ++i)
            sum += (double) played[i] * played[i];

        return std::sqrt (sum / (double) (played.size() - 12000));
    };

    CHECK (rmsThrough (devices::dsp::FilterMode::notch, 440.0f) < 0.05 * rmsThrough (devices::dsp::FilterMode::lowPass, 20000.0f));
}

TEST_CASE ("A note whose cutoff Envelope 2 opens with no attack plays its filter open from its first sample")
{
    // A saw through a 200 Hz low-pass that Envelope 2 opens two octaves at once and holds there, against the same saw through
    // the filter set at 800 Hz, still: the two alike from the note's start, not darker for its first samples.
    auto values = getPlainValues (2.0f / 3.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::resonance] = 0.0f;
    values[Synth::keyTracking] = 0.0f;
    values[Synth::env2Attack] = 0.0f;
    values[Synth::env2Sustain] = 1.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::envelope2;
    values[Synth::mod1Target] = (float) Synth::ModTarget::cutoff;
    const auto playThrough = [&values] (float cutoffHertz, float amount)
    {
        values[Synth::cutoff] = cutoffHertz;
        values[Synth::mod1Amount] = amount;
        return play (*makeSynth(), values, 1024, { noteOn (100, 60) }); // between two control steps, as a song's notes are
    };

    const auto opened = playThrough (200.0f, 24.0f / 96.0f), still = playThrough (800.0f, 0.0f);
    REQUIRE (peakOf (still.left, 100, 132) > 0.01f);
    CHECK (largestDifference (opened, still) < 1.0e-6f);
}

TEST_CASE ("A driven synth leaves nothing off centre, even of a synced or a narrow wave")
{
    // The filter's drive bends a wave that is centred but lopsided unevenly, which leaves an offset: the synth plays its
    // blocker while the drive is on, which takes it away.
    const auto offsetOf = [] (Synth::Warp warp, float amount, float driveDecibels)
    {
        auto values = getPlainValues (2.0f / 3.0f); // a saw
        values[Synth::osc1Warp] = (float) warp;
        values[Synth::osc1WarpAmount] = amount;
        values[Synth::filterOn] = 1.0f;
        values[Synth::cutoff] = 12000.0f;
        values[Synth::drive] = driveDecibels;
        const auto played = play (*makeSynth(), values, 96000, { noteOn (0, 57) }).left;
        auto sum = 0.0;

        for (size_t i = 48000; i < played.size(); ++i)
            sum += played[i];

        return std::abs (sum / 48000.0);
    };

    for (const auto& [warp, amount] : { std::pair { Synth::Warp::none, 0.0f }, std::pair { Synth::Warp::sync, 0.4f },
                                        std::pair { Synth::Warp::pulseWidth, 0.6f }, std::pair { Synth::Warp::bendUp, 0.7f } })
    {
        const auto driven = offsetOf (warp, amount, 18.0f);
        CAPTURE ((int) warp);
        CHECK (driven < 1.0e-5); // it measures 3e-7
    }
}

TEST_CASE ("The synth's filter drive saturates what goes into the filter, adding harmonics to a sine, and only while the filter is on")
{
    // A sine through the open filter: driven 18 dB, it gains its third harmonic; at 0 dB, it plays as before, bit for bit.
    auto values = getPlainValues (0.0f);
    values[Synth::filterOn] = 1.0f;
    values[Synth::cutoff] = 20000.0f;
    values[Synth::resonance] = 0.0f;
    values[Synth::keyTracking] = 0.0f;
    const auto thirdOf = [] (const Played& played)
    { return measureLevelAt (played.left, 4800, 24000, 660.0) / measureLevelAt (played.left, 4800, 24000, 220.0); };

    const auto plain = play (*makeSynth(), values, 24000, { noteOn (0, 57) });
    CHECK (thirdOf (plain) < 0.001);

    values[Synth::drive] = 18.0f;
    CHECK (thirdOf (play (*makeSynth(), values, 24000, { noteOn (0, 57) })) > 0.05);

    values[Synth::drive] = 0.0f;
    CHECK (play (*makeSynth(), values, 24000, { noteOn (0, 57) }).left == plain.left);

    // With the filter switched off, the drive is off with it.
    values[Synth::drive] = 18.0f;
    values[Synth::filterOn] = 0.0f;
    CHECK (thirdOf (play (*makeSynth(), values, 24000, { noteOn (0, 57) })) < 0.001);
}

TEST_CASE ("The synth's drive fades in over the same time at 96 kHz as at 48 kHz")
{
    // What the drive changes in a note (driven less not driven) as it starts: over its first third of a millisecond against
    // the third of a millisecond a millisecond in. It fades in over two thirds of one at any rate, so the two are alike.
    const auto fadedIn = [] (double rate)
    {
        auto values = getPlainValues (0.0f);
        values[Synth::filterOn] = 1.0f;
        values[Synth::cutoff] = 20000.0f;
        values[Synth::resonance] = 0.0f;
        values[Synth::keyTracking] = 0.0f;
        values[Synth::env1Attack] = 0.5f;
        Synth driven, plain;
        driven.prepare (rate, testBlockSize);
        plain.prepare (rate, testBlockSize);
        const auto withoutDrive = play (plain, values, (int) (0.01 * rate), { noteOn (0, 81) }).left;
        values[Synth::drive] = 18.0f;
        const auto withDrive = play (driven, values, (int) (0.01 * rate), { noteOn (0, 81) }).left;
        const auto third = (size_t) (rate / 3000.0);
        auto early = 0.0, later = 0.0;

        for (size_t i = 0; i < third; ++i)
        {
            early += std::abs (withDrive[i] - withoutDrive[i]);
            later += std::abs (withDrive[i + 3 * third] - withoutDrive[i + 3 * third]);
        }

        return early / later;
    };

    const auto at48 = fadedIn (48000.0), at96 = fadedIn (96000.0);
    CAPTURE (at48);
    CAPTURE (at96);
    CHECK (at96 == doctest::Approx (at48).epsilon (0.15));
}

TEST_CASE ("The synth's noise is as loud in the band that is heard at any sample rate, as high its peaks, driven or not")
{
    // The noise: what a note plays with it, less what it plays without, at 44.1, 96 and 192 kHz, through a 12 dB low-pass at
    // 1 kHz, and driven hard before it. Its power below 20 kHz, from its spectrum, and its true peak (ITU-R BS.1770: as a
    // converter rebuilds it between its samples, devices/dynamics/TruePeak.h), against 48 kHz's. White noise at the device's
    // own rate would spread over the band up to half the rate (at 96 kHz less than half of it heard), so the noise is made
    // at 44.1 or 48 kHz, as loud for each hertz as at 44.1 kHz, and raised to the rate (devices/dsp/Noise.h). Driven, what
    // the drive leaves below the cutoff grows with all the noise that goes in, and the drive works a little differently at
    // each rate: within a decibel.
    struct Heard
    {
        double power = 0.0, peak = 0.0;
    };

    const auto heardAt = [] (double rate, bool driven)
    {
        auto values = getPlainValues (0.0f);
        values[Synth::noiseLevel] = 0.5f;
        values[Synth::filterOn] = 1.0f;
        values[Synth::filterSlope] = 0.0f; // 12 dB
        values[Synth::cutoff] = 1000.0f;
        values[Synth::resonance] = 0.0f;
        values[Synth::keyTracking] = 0.0f;
        values[Synth::drive] = driven ? 18.0f : 0.0f;
        Synth with, without;
        with.prepare (rate, testBlockSize);
        without.prepare (rate, testBlockSize);
        const auto order = rate > 100000.0 ? 17 : (rate > 50000.0 ? 16 : 15);
        const auto size = 1 << order;
        const auto noisy = play (with, values, size, { noteOn (0, 45) }).left;
        values[Synth::noiseLevel] = 0.0f;
        const auto clean = play (without, values, size, { noteOn (0, 45) }).left;

        std::vector<float> data ((size_t) (2 * size), 0.0f);
        Heard heard;
        devices::TruePeak truePeak;

        for (size_t i = 0; i < (size_t) size; ++i)
        {
            data[i] = noisy[i] - clean[i];
            heard.peak = std::max ({ heard.peak, (double) std::abs (data[i]), (double) truePeak.push (data[i]) });
        }

        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (data.data());

        for (int bin = 1; bin < size / 2; ++bin)
            if (bin * rate / size < 20000.0)
                heard.power += (double) data[(size_t) bin] * data[(size_t) bin];

        heard.power /= (double) size * size;
        return heard;
    };

    for (const auto driven : { false, true })
    {
        const auto reference = heardAt (48000.0, driven);

        for (const auto rate : { 44100.0, 96000.0, 192000.0 })
        {
            const auto heard = heardAt (rate, driven);
            const auto level = 10.0 * std::log10 (heard.power / reference.power), peak = 20.0 * std::log10 (heard.peak / reference.peak);
            INFO (std::string (driven ? "driven, " : "not driven, ")
                  << rate << " Hz against 48 kHz: " << level << " dB heard, peak " << peak << " dB");
            CHECK (std::abs (level) < (driven ? 1.0 : 0.5));
            CHECK (std::abs (peak) < 1.5);
        }
    }
}

TEST_CASE ("The synth's Drift source wanders at the same pace at 96 kHz as at 48 kHz")
{
    // Drift on the first oscillator's pitch, at eight cents, from the same seed: the cents a note is off, measured over a few
    // stretches at each rate, alike.
    const auto centsOff = [] (double rate)
    {
        auto values = getPlainValues (0.0f);
        values[Synth::mod2Source] = (float) Synth::ModSource::drift;
        values[Synth::mod2Target] = (float) Synth::ModTarget::osc1Pitch;
        values[Synth::mod2Amount] = 0.08f / 24.0f;
        Synth synth;
        synth.prepare (rate, testBlockSize);
        const auto played = play (synth, values, (int) (1.2 * rate), { noteOn (0, 69) }).left;
        std::vector<double> cents;

        for (const auto from : { 0.1, 0.4, 0.7, 1.0 })
        {
            const auto first = (size_t) (from * rate), last = (size_t) ((from + 0.1) * rate);
            auto firstCrossing = 0.0, lastCrossing = 0.0;
            auto crossings = 0;

            for (auto i = first + 1; i < last; ++i)
            {
                if (played[i - 1] < 0.0f && played[i] >= 0.0f)
                {
                    // Where between the samples it crosses.
                    const auto at = (double) (i - 1) + played[i - 1] / (played[i - 1] - played[i]);

                    if (crossings == 0)
                        firstCrossing = at;

                    lastCrossing = at;
                    ++crossings;
                }
            }

            const auto hertz = (crossings - 1) * rate / (lastCrossing - firstCrossing);
            cents.push_back (1200.0 * std::log2 (hertz / 440.0));
        }

        return cents;
    };

    const auto at48 = centsOff (48000.0), at96 = centsOff (96000.0);
    auto moved = false;

    for (size_t i = 0; i < at48.size(); ++i)
    {
        CAPTURE (i);
        CAPTURE (at48[i]);
        CAPTURE (at96[i]);
        CHECK (std::abs (at48[i] - at96[i]) < 0.5);
        moved = moved || std::abs (at48[i]) > 0.5;
    }

    CHECK (moved); // it wanders at all
}

namespace
{
// A preset that comes with Phazite (resources/presets/Synth): each parameter it names, a choice by its name, the others at
// their defaults.
std::vector<float> readSynthPreset (const char* path)
{
    auto values = getDefaults();
    const auto file = phazite::test::getFactoryPresetsFolder().getChildFile ("Synth").getChildFile (path);
    const auto json = juce::JSON::parse (file);
    const auto* set = json["device"]["parameters"].getDynamicObject();
    REQUIRE (set != nullptr);

    for (const auto& property : set->getProperties())
    {
        const auto id = property.name.toString().toStdString();

        for (size_t index = 0; index < Synth::info.parameters.size(); ++index)
        {
            const auto& parameter = Synth::info.parameters[index];

            if (parameter.id != id)
                continue;

            if (property.value.isString())
            {
                const auto name = property.value.toString().toStdString();

                for (size_t choice = 0; choice < parameter.choices.size(); ++choice)
                    if (parameter.choices[choice] == name)
                        values[index] = (float) choice;
            }
            else
            {
                values[index] = (float) (double) property.value;
            }
        }
    }

    return values;
}

// The samples' bits, both sides, as one number (FNV-1a).
std::uint64_t hashOf (const Played& played)
{
    auto hash = (std::uint64_t) 14695981039346656037ull;

    for (const auto* side : { &played.left, &played.right })
    {
        for (const auto sample : *side)
        {
            hash ^= (std::uint64_t) std::bit_cast<std::uint32_t> (sample);
            hash *= (std::uint64_t) 1099511628211ull;
        }
    }

    return hash;
}

// What the synth works out once a control step, moved by a 25 Hz LFO at full amount (the zipper's measure).
enum class Moved
{
    volume,
    pan,
    pitch,
    level,
    position,
    warp,
    warpWithin, // the warp's amount at half: from 0 to 100 %, never cut off at the end of its range
    cutoff,     // a 24 dB low-pass's, at a Resonance of 80 %, from the note, two octaves each way
    resonance,  // the same filter's, from 60 to 100 %
    subPitch    // the pitch, the sub alone playing (a sine, at the note)
};

// A sine's note at 1 kHz (B5 tuned up by its Fine) at 44.1 kHz, with LFO 1 (a sine at 25 Hz) moving one thing at full
// amount, or nothing: what the left side plays from a quarter of a second on, 2^17 samples.
std::vector<float> playZipperNote (Moved moved, bool moving, Synth::Quality quality = Synth::Quality::once)
{
    constexpr double rate = 44100.0;
    auto values = getPlainValues (0.0f);
    values[Synth::osc1Fine] = (float) (1200.0 * std::log2 (1000.0 / noteFrequency (83)));
    values[Synth::lfo1Shape] = 0.0f; // a sine
    values[Synth::lfo1Rate] = 25.0f;
    values[Synth::lfo1Sync] = 0.0f;
    values[Synth::mod1Source] = moving ? (float) Synth::ModSource::lfo1 : 0.0f;
    values[Synth::mod1Amount] = 1.0f;
    values[Synth::quality] = (float) quality;
    Synth synth;
    synth.prepare (rate, testBlockSize);

    const auto target = [&]
    {
        switch (moved)
        {
            case Moved::volume:
                return Synth::ModTarget::volume;
            case Moved::pan:
                return Synth::ModTarget::pan;
            case Moved::pitch:
                return Synth::ModTarget::pitch;
            case Moved::level:
                values[Synth::osc1Level] = 0.5f;
                return Synth::ModTarget::osc1Level;
            case Moved::position:
            {
                // Four frames, each the sine a quarter of a cycle on from the last: morphing between them moves only the
                // fundamental, so that the note has no other harmonics.
                std::vector<Wavetable::Harmonics> frames;

                for (int quarter = 0; quarter < 4; ++quarter)
                    frames.push_back ({ std::polar (1.0f, 1.5707963f * (float) quarter) });

                synth.setTable (0, Wavetable::fromHarmonics ("Quarters", frames));
                values[Synth::osc1Position] = 0.5f;
                return Synth::ModTarget::osc1Position;
            }
            case Moved::warp:
            case Moved::warpWithin:
                values[Synth::osc1Warp] = (float) Synth::Warp::sync;
                values[Synth::osc1WarpAmount] = 0.5f;
                values[Synth::mod1Amount] = moved == Moved::warp ? 1.0f : 0.5f;
                return Synth::ModTarget::osc1Warp;
            case Moved::cutoff:
            case Moved::resonance:
                values[Synth::filterOn] = 1.0f;
                values[Synth::filterType] = (float) devices::dsp::FilterMode::lowPass;
                values[Synth::filterSlope] = 1.0f; // 24 dB
                values[Synth::cutoff] = 1000.0f;
                values[Synth::resonance] = 0.8f;
                values[Synth::keyTracking] = 0.0f;
                values[Synth::mod1Amount] = moved == Moved::cutoff ? 0.25f : 0.2f; // 24 of the 96 semitones; 20 %
                return moved == Moved::cutoff ? Synth::ModTarget::cutoff : Synth::ModTarget::resonance;
            case Moved::subPitch:
                values[Synth::osc1On] = 0.0f;
                values[Synth::subLevel] = 1.0f;
                values[Synth::subShape] = 0.0f; // a sine
                return Synth::ModTarget::pitch;
        }

        return Synth::ModTarget::none;
    }();

    values[Synth::mod1Target] = (float) target;
    const auto skip = (int) (0.25 * rate), length = 1 << 17;
    // The sub an octave under the key, which its Fine doesn't tune: at B5, 988 Hz.
    const auto played = play (synth, values, skip + length, { noteOn (0, moved == Moved::subPitch ? 95 : 83) });
    return { played.left.begin() + skip, played.left.end() };
}

// The amplitudes of a sound's spectrum, through a 7-term Blackman-Harris window (its sidelobes far under anything measured
// here), and the hertz between its bins.
std::vector<double> getAmplitudes (const std::vector<float>& sound, double rate, double& binHertz)
{
    const auto order = juce::roundToInt (std::log2 ((double) sound.size()));
    const auto size = (size_t) 1 << order;
    constexpr double a[] = { 0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
                             0.01081174209837, 0.00077658482522, 0.00001388721735 };
    std::vector<float> data (2 * size, 0.0f);
    auto weights = 0.0;

    for (size_t n = 0; n < size; ++n)
    {
        auto weight = 0.0;

        for (size_t k = 0; k < std::size (a); ++k)
            weight += (k % 2 == 0 ? 1.0 : -1.0) * a[k] * std::cos (twoPi * (double) k * (double) n / (double) size);

        data[n] = (float) (weight * sound[n]);
        weights += weight;
    }

    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data());
    std::vector<double> amplitudes (size / 2);

    for (size_t bin = 0; bin < amplitudes.size(); ++bin)
        amplitudes[bin] = 2.0 * (double) data[bin] / weights;

    binHertz = rate / (double) size;
    return amplitudes;
}

// The zipper: the loudest component from 50 Hz to 15 kHz that is off the grid of 25 Hz the note's harmonics and the LFO's
// sidebands about them fall on (aliases folded back from 44.1 kHz included), in dB under the note as it plays unmoved. The
// grid is the note's harmonics, at 1 kHz, but for the pitch moved, whose sidebands are about the mean of the frequencies it
// moves through: so it is laid through the loudest component. The control steps' images fall between: at 44.1 kHz a step is
// 2756.25 Hz, 110.25 times 25 Hz, so each of its first three multiples lands a quarter, a half or three quarters of the way
// between two lines of the grid (the fourth, 11,025 Hz, falls on it, and the fifth a quarter off again).
double measureZipper (Moved moved, Synth::Quality quality = Synth::Quality::once)
{
    constexpr double rate = 44100.0;
    auto binHertz = 0.0;
    const auto still = getAmplitudes (playZipperNote (moved, false, quality), rate, binHertz);
    const auto noteBin = (size_t) std::lround ((moved == Moved::subPitch ? noteFrequency (83) : 1000.0) / binHertz);
    const auto note = *std::max_element (still.begin() + (long) noteBin - 10, still.begin() + (long) noteBin + 10);
    const auto amplitudes = getAmplitudes (playZipperNote (moved, true, quality), rate, binHertz);
    const auto first = (size_t) (50.0 / binHertz), last = (size_t) (15000.0 / binHertz);

    // Where the loudest component is, between its bins (a parabola through the decibels of the three about it).
    const auto peak =
        (size_t) std::distance (amplitudes.begin(), std::max_element (amplitudes.begin() + (long) first, amplitudes.begin() + (long) last));
    const auto a = std::log (amplitudes[peak - 1]), b = std::log (amplitudes[peak]), c = std::log (amplitudes[peak + 1]);
    const auto peakHertz = ((double) peak + 0.5 * (a - c) / (a - 2.0 * b + c)) * binHertz;
    const auto offGrid = [peakHertz] (double hertz)
    {
        const auto from = hertz - peakHertz;
        return std::abs (from - 25.0 * std::round (from / 25.0)) > 2.6;
    };

    auto loudest = 0.0;

    for (auto bin = first; bin <= last; ++bin)
        if (offGrid ((double) bin * binHertz))
            loudest = std::max (loudest, amplitudes[bin]);

    return 20.0 * std::log10 (std::max (loudest, 1.0e-12) / note);
}
} // namespace

TEST_CASE ("A synth note with nothing moving plays bit for bit what it played before its modulation glided")
{
    // Three presets, the defaults, a warped patch and two other filters, each held still, played as a chord held for half a
    // second and let go: the same samples as the synth played before what its modulation moves, its filter and its sub's
    // pitch went in straight lines across each control step, as hashes of them taken then. A line from a value to itself
    // adds nothing, bit for bit. Clean Sub's hash was taken before the oscillators' lines; the others' (their filters held
    // still: in the defaults, Hypersaw Lead and Chord Stab, Envelope 2 opens the cutoff) before the filter's and the sub's,
    // from code that played anything still as the code before the oscillators' lines did. Taken with MSVC: another
    // compiler's maths library rounds its sines and powers its own way, so the hashes are this one's.
    auto warped = getDefaults();
    warped[Synth::osc1Warp] = (float) Synth::Warp::sync;
    warped[Synth::osc1WarpAmount] = 0.4f;
    warped[Synth::osc2On] = 1.0f;
    warped[Synth::osc2Warp] = (float) Synth::Warp::bendUp;
    warped[Synth::osc2WarpAmount] = 0.3f;
    warped[Synth::unisonVoices] = 2.0f; // three
    warped[Synth::subLevel] = 0.4f;
    warped[Synth::noiseLevel] = 0.1f;

    struct Patch
    {
        const char* name;
        std::vector<float> values;
        std::uint64_t hash; // of what it played before
    };

    // Their filters held still: the first slot, Envelope 2 on the cutoff in each, off (Chord Stab's Velocity slot on the
    // cutoff stays: a note's velocity doesn't move). And two other types of filter, held still.
    const auto still = [] (std::vector<float> values)
    {
        values[Synth::mod1Source] = (float) Synth::ModSource::none;
        return values;
    };

    auto highPass = still (getDefaults());
    highPass[Synth::filterType] = (float) devices::dsp::FilterMode::highPass;
    highPass[Synth::filterSlope] = 1.0f; // 24 dB
    highPass[Synth::resonance] = 0.6f;
    highPass[Synth::cutoff] = 400.0f;
    auto bandPass = still (getDefaults());
    bandPass[Synth::filterType] = (float) devices::dsp::FilterMode::bandPass;
    bandPass[Synth::filterSlope] = 0.0f; // 12 dB
    bandPass[Synth::resonance] = 0.5f;

    const Patch patches[] = {
        { "defaults", still (getDefaults()), 16800395846176970661ull },
        { "Clean Sub", readSynthPreset ("Bass/Clean Sub.phazitepreset"), 9052229557940170989ull },
        { "Hypersaw Lead", still (readSynthPreset ("Lead/Hypersaw Lead.phazitepreset")), 10207463208611763461ull },
        { "Chord Stab", still (readSynthPreset ("Pluck/Chord Stab.phazitepreset")), 856507992685815535ull },
        { "warped", still (warped), 6477364409459061684ull },
        { "high-pass", highPass, 16963838326320298349ull },
        { "band-pass", bandPass, 2936370508618044397ull },
    };

    // At Quality 1x, as late as every Quality is (Synth::getLatency()): its samples from there on, and silence before them.
    const auto latency = Synth::getLatency();

    for (const auto& [name, values, hash] : patches)
    {
        // In mono mode one note: the next would move its pitch, legato.
        const auto mono = values[Synth::voiceMode] > 0.5f;
        auto played = play (*makeSynth(), values, 48000 + latency,
                            mono ? std::vector { noteOn (0, 48, 0.8f), noteOff (24000, 48) }
                                 : std::vector { noteOn (0, 48, 0.8f), noteOn (0, 55, 0.8f), noteOn (100, 60, 0.8f), noteOff (24000, 48),
                                                 noteOff (24000, 55), noteOff (24000, 60) });

        for (auto* side : { &played.left, &played.right })
        {
            CHECK_FALSE (peakOf (*side, 0, (size_t) latency) > 0.0f);
            side->erase (side->begin(), side->begin() + latency);
        }

        INFO (std::string (name) << ": " << hashOf (played));
        CHECK (peakOf (played.left, 0, played.left.size()) > 0.01f);
#if JUCE_MSVC
        CHECK (hashOf (played) == hash);
#else
        juce::ignoreUnused (hash);
#endif
    }
}

TEST_CASE ("What the synth's modulation moves glides across each control step: no zipper under the note")
{
    // A sine's note at 1 kHz with a 25 Hz LFO at full amount on each in turn: what the control steps left under it as they
    // stepped was 47 to 12 dB under the note (Volume -46.7, Pan -49.5, Pitch -63.4, Level -45.0, Position -39.4, Sync's amount
    // -11.6). Gliding, it is the line's own error, the LFO between two steps being a curve, and where the LFO is cut off at
    // the end of its range (Level, Position, Warp), the corner a step's line cuts. Sync is the most sensitive to it: its
    // place in the wave is the note's phase times its ratio, which a line between steps misses by a little at every curve of
    // the ratio, even with the LFO at half, within the amount's range. Each held to what it measures less 6 dB.
    struct Expected
    {
        Moved moved;
        const char* name;
        double measured; // dB under the note
    };

    for (const auto& [moved, name, measured] :
         { Expected { Moved::volume, "Volume", -87.4 }, Expected { Moved::pan, "Pan", -90.2 }, Expected { Moved::pitch, "Pitch", -94.6 },
           Expected { Moved::level, "Level", -77.2 }, Expected { Moved::position, "Position", -65.3 },
           Expected { Moved::warp, "Sync's amount", -30.9 }, Expected { Moved::warpWithin, "Sync's amount, within its range", -40.3 } })
    {
        const auto zipper = measureZipper (moved);
        MESSAGE (std::string (name) << ": " << zipper << " dB under the note");
        CHECK (zipper < measured + 6.0);
    }
}

TEST_CASE ("The synth's filter and its sub's pitch glide across each control step: no zipper under the note")
{
    // The same measure, a 25 Hz LFO on a 24 dB low-pass's cutoff at a Resonance of 80 %, by two octaves each way from the
    // note; on its Resonance, from 60 to 100 %; and on the pitch with the sub alone playing. Stepping, they left -69.9,
    // -79.7 and -63.4 dB under the note. Each held to what it measures less 6 dB.
    struct Expected
    {
        Moved moved;
        const char* name;
        double measured; // dB under the note
    };

    for (const auto& [moved, name, measured] :
         { Expected { Moved::cutoff, "Cutoff", -106.2 }, Expected { Moved::resonance, "Resonance", -121.0 },
           Expected { Moved::subPitch, "Sub's pitch", -95.1 } })
    {
        const auto zipper = measureZipper (moved);
        MESSAGE (std::string (name) << ": " << zipper << " dB under the note");
        CHECK (zipper < measured + 6.0);
    }
}

TEST_CASE ("The synth's sync zipper at each Quality")
{
    // The first glides' two worst rows, Sync's amount moved by the LFO at full and at half, at 2x and 4x, where the control
    // steps are as many samples at a higher rate. At 4x the steps' images (11,025 Hz apart) fall on the measure's grid of 25 Hz,
    // so what it finds there is what is left between them. Each held to what it measures less 6 dB.
    struct Expected
    {
        Moved moved;
        Synth::Quality quality;
        double measured; // dB under the note
    };

    for (const auto& [moved, quality, measured] :
         { Expected { Moved::warp, Synth::Quality::twice, -41.5 }, Expected { Moved::warpWithin, Synth::Quality::twice, -53.0 },
           Expected { Moved::warp, Synth::Quality::fourTimes, -118.1 }, Expected { Moved::warpWithin, Synth::Quality::fourTimes, -114.2 } })
    {
        const auto zipper = measureZipper (moved, quality);
        MESSAGE (std::string (moved == Moved::warp ? "Sync's amount" : "Sync's amount, within its range")
                 << " at " << Synth::getFactor (quality) << "x: " << zipper << " dB under the note");
        CHECK (zipper < measured + 6.0);
    }
}

namespace
{
// A sine's note, Envelope 1 shaping it, LFO 1 on its Volume and a glide in mono mode from A3 to A4, at a Quality: what it
// plays, from where its latency ends.
Played playTimedPatch (Synth::Quality quality)
{
    auto values = getPlainValues (0.0f); // Basic Shapes' sine
    values[Synth::env1Attack] = 30.0f;
    values[Synth::env1Decay] = 150.0f;
    values[Synth::env1Sustain] = 0.5f;
    values[Synth::env1Release] = 100.0f;
    values[Synth::lfo1Rate] = 4.0f;
    values[Synth::mod1Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod1Target] = (float) Synth::ModTarget::volume;
    values[Synth::mod1Amount] = 0.3f;
    values[Synth::voiceMode] = 1.0f; // mono
    values[Synth::glide] = 120.0f;
    values[Synth::quality] = (float) quality;
    const auto latency = Synth::getLatency();
    auto played =
        play (*makeSynth(), values, 48000 + latency, { noteOn (0, 57), noteOn (19200, 69), noteOff (38400, 57), noteOff (38400, 69) });
    played.left.erase (played.left.begin(), played.left.begin() + latency);
    return played;
}

// A sound's level, in decibels, in each 10 ms of it.
std::vector<double> getLevelsEvery10ms (const std::vector<float>& sound)
{
    std::vector<double> levels;
    constexpr size_t length = 480;

    for (size_t from = 0; from + length <= sound.size(); from += length)
    {
        auto sum = 0.0;

        for (auto i = from; i < from + length; ++i)
            sum += (double) sound[i] * (double) sound[i];

        levels.push_back (10.0 * std::log10 (std::max (sum / (double) length, 1.0e-20)));
    }

    return levels;
}

// A sound's frequency in each 20 ms of it, from its rising zero crossings, between samples.
std::vector<double> getFrequenciesEvery20ms (const std::vector<float>& sound)
{
    std::vector<double> frequencies;
    constexpr size_t length = 960;

    for (size_t from = 1; from + length <= sound.size(); from += length)
    {
        auto first = -1.0, last = -1.0;
        auto crossings = 0;

        for (auto i = from; i < from + length; ++i)
        {
            if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
            {
                last = (double) (i - 1) + (double) sound[i - 1] / (double) (sound[i - 1] - sound[i]);
                first = crossings++ == 0 ? last : first;
            }
        }

        frequencies.push_back (crossings > 1 ? (crossings - 1) * testRate / (last - first) : 0.0);
    }

    return frequencies;
}

// The rms level of a stretch of a sound, in decibels.
double getLevelOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += (double) sound[i] * (double) sound[i];

    return 10.0 * std::log10 (sum / (double) (to - from));
}
} // namespace

TEST_CASE ("The synth's Quality: at 2x and 4x its envelope, LFO, glide and pitch keep their times in seconds, as late as at 1x")
{
    // Every Quality as late as the latest, so that it can change while the song plays.
    auto synth = makeSynth();
    CHECK (synth->getLatencySamples() == Synth::getLatency());
    CHECK (Synth::getLatency() > 0);

    const auto once = playTimedPatch (Synth::Quality::once);
    const auto levels = getLevelsEvery10ms (once.left);
    const auto frequencies = getFrequenciesEvery20ms (once.left);

    for (const auto quality : { Synth::Quality::twice, Synth::Quality::fourTimes })
    {
        CAPTURE (Synth::getFactor (quality));
        const auto played = playTimedPatch (quality);
        const auto otherLevels = getLevelsEvery10ms (played.left);
        const auto otherFrequencies = getFrequenciesEvery20ms (played.left);
        auto mostOff = 0.0, mostCents = 0.0;

        // Its level every 10 ms within 0.1 dB, wherever it sounds (the envelope's shape, the LFO's rate and phase).
        for (size_t i = 0; i < levels.size(); ++i)
            if (levels[i] > -60.0)
                mostOff = std::max (mostOff, std::abs (otherLevels[i] - levels[i]));

        // Its pitch every 20 ms within a cent (the glide's time, the note's pitch), wherever the note sounds all through them
        // (not where it ends in the middle of one, whose crossings stop there).
        for (size_t i = 0; i < frequencies.size(); ++i)
            if (frequencies[i] > 0.0 && levels[2 * i] > -40.0 && levels[2 * i + 1] > -40.0)
                mostCents = std::max (mostCents, std::abs (1200.0 * std::log2 (otherFrequencies[i] / frequencies[i])));

        // As late as at 1x: the held note's waves line up within a sixteenth of a sample (by the cross-correlation's
        // peak, between samples by a straight line).
        auto best = 0.0, bestShift = 0.0;

        for (auto shift = -4.0; shift <= 4.0; shift += 1.0 / 16.0)
        {
            auto sum = 0.0;

            for (size_t n = 9600; n < 18000; ++n)
            {
                const auto at = (double) n + shift;
                const auto whole = (size_t) std::floor (at);
                const auto along = at - std::floor (at);
                sum += (double) once.left[n] * ((1.0 - along) * played.left[whole] + along * played.left[whole + 1]);
            }

            if (sum > best)
            {
                best = sum;
                bestShift = shift;
            }
        }

        CAPTURE (bestShift);
        CHECK (std::abs (bestShift) < 0.1);
        CAPTURE (mostOff);
        CAPTURE (mostCents);
        CHECK (mostOff < 0.1);
        CHECK (mostCents < 1.0);
    }
}

TEST_CASE ("The synth's Quality: a plain saw, the sub and the noise are as loud at 1x, 2x and 4x")
{
    struct Sound
    {
        const char* name;
        std::vector<float> values;
    };

    auto saw = getPlainValues (2.0f / 3.0f);
    auto sub = getPlainValues (0.0f);
    sub[Synth::osc1On] = 0.0f;
    sub[Synth::subLevel] = 1.0f;
    auto noise = sub;
    noise[Synth::subLevel] = 0.0f;
    noise[Synth::noiseLevel] = 1.0f;

    for (const auto& [name, values] : { Sound { "saw", saw }, Sound { "sub", sub }, Sound { "noise", noise } })
    {
        CAPTURE (name);
        auto atOnce = 0.0;

        for (const auto quality : { Synth::Quality::once, Synth::Quality::twice, Synth::Quality::fourTimes })
        {
            auto withQuality = values;
            withQuality[Synth::quality] = (float) quality;
            const auto played = play (*makeSynth(), withQuality, 48000, { noteOn (0, 60) });
            const auto level = getLevelOf (played.left, 4800, 48000);

            if (quality == Synth::Quality::once)
                atOnce = level;

            CAPTURE (Synth::getFactor (quality));
            CAPTURE (level - atOnce);
            CHECK (std::abs (level - atOnce) < 0.1);
        }
    }
}

TEST_CASE ("The synth's Quality changed while a chord is held: its voices fade out, the change waits, and nothing clicks")
{
    auto values = getDefaults();
    auto synth = makeSynth();
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> played;
    std::vector<int> qualities;

    // A chord held for half a second at 1x; at 0.25 s the Quality goes to 2x, and a note played 1 ms after it (the chord's
    // lowest again) waits for the change, then plays.
    const auto changeAt = 12032, lateNote = changeAt + 48; // changeAt at a block's start

    for (int done = 0; done < 24064; done += testBlockSize)
    {
        buffer.clear();
        std::vector<NoteEvent> events;

        if (done == 0)
            events = { noteOn (0, 48), noteOn (0, 55), noteOn (0, 60) };

        if (lateNote >= done && lateNote < done + testBlockSize)
            events.push_back (noteOn (lateNote - done, 48));

        values[Synth::quality] = done >= changeAt ? 1.0f : 0.0f;
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = testBlockSize;
        block.parameters = values;
        block.events = events;
        synth->process (block);
        qualities.push_back ((int) synth->getPlayingQuality());
        played.insert (played.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + testBlockSize);
    }

    const auto largestJump = [&played] (size_t from, size_t to)
    {
        auto most = 0.0f;

        for (auto i = from + 1; i < to; ++i)
            most = std::max (most, std::abs (played[i] - played[i - 1]));

        return most;
    };

    // The chord's own largest step between samples, held, against the steps about the change, from a block before it to
    // 5 ms after it was asked for (the fade, the wait, and the late note's attack, which is 2 ms): no larger. (After it the
    // late note at 2x, a saw with more of its top, steps a little further between samples than the chord at 1x did.)
    const auto own = largestJump (4800, (size_t) changeAt);
    const auto aboutTheChange = largestJump ((size_t) (changeAt - testBlockSize), (size_t) changeAt + 240);
    CAPTURE (own);
    CAPTURE (aboutTheChange);
    CHECK (aboutTheChange <= own);

    // It changed after the fade, the latency and the filters' settling (within the block it was asked for), and the late
    // note sounds.
    CHECK (qualities[(size_t) (changeAt / testBlockSize) - 1] == (int) Synth::Quality::once);
    CHECK (qualities.back() == (int) Synth::Quality::twice);
    CHECK (synth->getActiveVoices() == 1);
    CHECK (peakOf (played, (size_t) changeAt + 2400, played.size()) > 0.05f);
}

TEST_CASE ("The synth at 2x and 4x plays the same samples in blocks of 7, 32 and 512")
{
    auto values = getDefaults();
    values[Synth::lfo1Rate] = 7.0f;
    values[Synth::mod2Source] = (float) Synth::ModSource::lfo1;
    values[Synth::mod2Target] = (float) Synth::ModTarget::osc1Warp;
    values[Synth::mod2Amount] = 0.5f;
    values[Synth::osc1Warp] = (float) Synth::Warp::sync;

    for (const auto quality : { Synth::Quality::twice, Synth::Quality::fourTimes })
    {
        CAPTURE (Synth::getFactor (quality));
        values[Synth::quality] = (float) quality;
        const std::vector events { noteOn (0, 48), noteOn (1000, 60), noteOff (9000, 48), noteOff (9100, 60) };
        const auto in512 = play (*makeSynth(), values, 14400, events, 512);

        for (const auto blockSize : { 7, 32 })
        {
            CAPTURE (blockSize);
            const auto other = play (*makeSynth(), values, 14400, events, blockSize);
            CHECK (other.left == in512.left);
            CHECK (other.right == in512.right);
        }
    }
}
