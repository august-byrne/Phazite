// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's wavetable synth (src/devices/wavetable/): its tables, kept band-limited at every pitch and read from sounds
// as Serum writes them, its factory tables, and the synth itself: its pitch, its position moving through the table (by
// hand and by an envelope for each note), its warps, unison, filter, combs and voices, that it doesn't alias, that it
// lets go of the tables it no longer plays, and that it plays the same every time.
#include "TestHelpers.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/Wavetable.h"
#include "devices/wavetable/WavetableSynth.h"

#include <array>
#include <cmath>
#include <set>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::NoteEvent;
using devices::Wavetable;
using devices::WavetableSynth;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : WavetableSynth::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// The first oscillator alone, on its table at full level: the filter off, no modulation, no velocity, full sustain.
std::vector<float> getPlainValues (float position)
{
    auto values = getDefaults();
    values[WavetableSynth::osc1Position] = position;
    values[WavetableSynth::osc1Level] = 1.0f;
    values[WavetableSynth::filterOn] = 0.0f;
    values[WavetableSynth::mod1Source] = 0.0f;
    values[WavetableSynth::env1Sustain] = 1.0f;
    values[WavetableSynth::velocity] = 0.0f;
    values[WavetableSynth::level] = 0.0f;
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
};

// Plays the synth in blocks, with events at their samples from the start.
Played play (WavetableSynth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    Played played;
    std::vector<NoteEvent> inBlock;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);
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
        synth.process (block);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

std::unique_ptr<WavetableSynth> makeSynth()
{
    auto synth = std::make_unique<WavetableSynth>();
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
} // namespace

TEST_CASE ("The wavetable synth's description holds together")
{
    REQUIRE (devices::isValid (WavetableSynth::info));
    CHECK (WavetableSynth::info.kind == devices::DeviceKind::instrument);
    CHECK (WavetableSynth::info.name == std::string_view ("Wavetable"));
    CHECK (WavetableSynth::info.parameters.size() == (size_t) WavetableSynth::level + 1);
    CHECK (devices::findParameter (WavetableSynth::info, "osc2Position") == WavetableSynth::osc2Position);
    CHECK (devices::findParameter (WavetableSynth::info, "mod8Amount") == WavetableSynth::mod8Amount);
    CHECK (devices::findParameter (WavetableSynth::info, "voiceMode") == WavetableSynth::voiceMode);
    CHECK (WavetableSynth::info.keyControls.size() == 2);
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

TEST_CASE ("The wavetable synth's factory tables have names of their own and make themselves once")
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

TEST_CASE ("A wavetable synth note plays at its pitch, the sine at the table's start")
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

TEST_CASE ("The wavetable synth's position morphs through its table, and an envelope moves it for each note")
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
    values[WavetableSynth::mod1Source] = (float) WavetableSynth::ModSource::envelope2;
    values[WavetableSynth::mod1Target] = (float) WavetableSynth::ModTarget::osc1Position;
    values[WavetableSynth::mod1Amount] = 1.0f;
    values[WavetableSynth::env2Attack] = 0.0f;
    values[WavetableSynth::env2Decay] = 200.0f;
    values[WavetableSynth::env2Sustain] = 0.0f;
    const std::vector<NoteEvent> events { noteOn (0, 45), noteOff (24000, 45), noteOn (28800, 45) };

    CHECK (fifthAt (0.0f, events, 0, 1200, values) > 0.1);       // bright as it starts
    CHECK (fifthAt (0.0f, events, 14400, 24000, values) < 0.01); // dark once it has decayed
    CHECK (fifthAt (0.0f, events, 28800, 30000, values) > 0.1);  // bright again with the next note
}

TEST_CASE ("The wavetable synth doesn't alias, however high it plays")
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

TEST_CASE ("The wavetable synth's warps reshape its wave, and sync's jump is smoothed")
{
    // Each warp at 70 % changes the sine at the table's start, without blowing up; FM and ring modulation need the other
    // oscillator.
    const auto plain = play (*makeSynth(), getPlainValues (0.0f), 9600, { noteOn (0, 57) });

    for (const auto warp :
         { WavetableSynth::Warp::sync, WavetableSynth::Warp::bendUp, WavetableSynth::Warp::bendDown, WavetableSynth::Warp::pulseWidth,
           WavetableSynth::Warp::mirror, WavetableSynth::Warp::fm, WavetableSynth::Warp::ringMod })
    {
        auto values = getPlainValues (0.0f);
        values[WavetableSynth::osc1Warp] = (float) warp;
        values[WavetableSynth::osc1WarpAmount] = 0.7f;
        values[WavetableSynth::osc2On] = 1.0f;
        values[WavetableSynth::osc2Level] = 0.0f; // heard only through the warps
        values[WavetableSynth::osc2Pitch] = 12.0f;
        values[WavetableSynth::osc2Position] = 0.0f;
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
    values[WavetableSynth::osc1Warp] = (float) WavetableSynth::Warp::sync;
    values[WavetableSynth::osc1WarpAmount] = 0.5f; // 2^1.5 times as fast
    const auto synced = play (*makeSynth(), values, 19200, { noteOn (0, 81) });
    const auto f0 = noteFrequency (81);
    auto strongest = 0.0;

    for (int harmonic = 1; harmonic <= 8; ++harmonic)
        strongest = std::max (strongest, measureLevelAt (synced.left, 4800, 19200, harmonic * f0));

    REQUIRE (strongest > 0.1);

    for (auto frequency = 0.5 * f0; frequency < 18000.0; frequency += f0)
        CHECK (measureLevelAt (synced.left, 4800, 19200, frequency) < 1.0e-3 * strongest);
}

TEST_CASE ("The wavetable synth's unison spreads its voices across the stereo field and its table")
{
    auto values = getPlainValues (2.0f / 3.0f);
    values[WavetableSynth::unisonVoices] = 3.0f; // four
    values[WavetableSynth::unisonDetune] = 30.0f;
    values[WavetableSynth::unisonSpread] = 1.0f;
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
    values[WavetableSynth::unisonSpread] = 0.0f;
    values[WavetableSynth::unisonDetune] = 0.0f;
    values[WavetableSynth::osc1Position] = 0.5f;
    auto synth = makeSynth();
    synth->setTable (0, devices::getFactoryWavetable (devices::findFactoryWavetable ("Saw Build")));
    const auto still = play (*synth, values, 9600, { noteOn (0, 45) });
    values[WavetableSynth::unisonPosition] = 1.0f;
    auto spreadSynth = makeSynth();
    spreadSynth->setTable (0, devices::getFactoryWavetable (devices::findFactoryWavetable ("Saw Build")));
    const auto moved = play (*spreadSynth, values, 9600, { noteOn (0, 45) });
    const auto sixtieth = 60.0 * noteFrequency (45);
    CHECK (measureLevelAt (still.left, 4800, 9600, sixtieth) < 1.0e-4);
    CHECK (measureLevelAt (moved.left, 4800, 9600, sixtieth) > 1.0e-3);
}

TEST_CASE ("The wavetable synth's filter darkens its saw, and the default patch opens it with each note")
{
    const auto tenthOf = [] (const std::vector<float>& values)
    {
        const auto played = play (*makeSynth(), values, 14400, { noteOn (0, 45) });
        return measureLevelAt (played.left, 4800, 14400, 10.0 * noteFrequency (45))
               / measureLevelAt (played.left, 4800, 14400, noteFrequency (45));
    };

    auto values = getPlainValues (2.0f / 3.0f);
    const auto open = tenthOf (values);
    values[WavetableSynth::filterOn] = 1.0f;
    values[WavetableSynth::filterType] = (float) WavetableSynth::FilterType::lowPass24;
    values[WavetableSynth::cutoff] = 400.0f;
    values[WavetableSynth::keyTracking] = 0.0f;
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

TEST_CASE ("The wavetable synth's combs: on a note's pitch, Comb + keeps a saw's harmonics and Comb - leaves out its even ones")
{
    // After the other types, so that those keep their numbers in saved projects.
    const auto& types = WavetableSynth::info.parameters[WavetableSynth::filterType].choices;
    REQUIRE (types.size() == 8);
    CHECK (types[(size_t) WavetableSynth::FilterType::notch] == std::string_view ("Notch"));
    CHECK (types[(size_t) WavetableSynth::FilterType::combPlus] == std::string_view ("Comb +"));
    CHECK (types[(size_t) WavetableSynth::FilterType::combMinus] == std::string_view ("Comb -"));

    // Basic Shapes' saw at A3 (220 Hz) through a comb tuned to it: its first four harmonics.
    auto values = getPlainValues (2.0f / 3.0f);
    values[WavetableSynth::filterOn] = 1.0f;
    values[WavetableSynth::cutoff] = 220.0f;
    values[WavetableSynth::resonance] = 0.5f;
    values[WavetableSynth::keyTracking] = 0.0f;
    const auto harmonicsThrough = [&values] (WavetableSynth::FilterType type)
    {
        values[WavetableSynth::filterType] = (float) type;
        auto synth = makeSynth();
        const auto played = play (*synth, values, 24000, { noteOn (0, 57) });
        std::array<double, 4> levels {};

        for (size_t h = 0; h < levels.size(); ++h)
            levels[h] = measureLevelAt (played.left, 4800, 24000, 220.0 * (double) (h + 1));

        return levels;
    };

    const auto plus = harmonicsThrough (WavetableSynth::FilterType::combPlus);
    CHECK (plus[1] / plus[0] == doctest::Approx (1.0 / 2.0).epsilon (0.1));
    CHECK (plus[2] / plus[0] == doctest::Approx (1.0 / 3.0).epsilon (0.1));
    CHECK (plus[3] / plus[0] == doctest::Approx (1.0 / 4.0).epsilon (0.1));

    const auto minus = harmonicsThrough (WavetableSynth::FilterType::combMinus);
    CHECK (minus[2] / minus[0] == doctest::Approx (1.0 / 3.0).epsilon (0.1));
    CHECK (minus[1] < 0.01 * minus[0]);
    CHECK (minus[3] < 0.01 * minus[0]);

    // Swept by an envelope for each note, it stays steady.
    values[WavetableSynth::mod1Source] = (float) WavetableSynth::ModSource::envelope2;
    values[WavetableSynth::mod1Target] = (float) WavetableSynth::ModTarget::cutoff;
    values[WavetableSynth::mod1Amount] = 1.0f;
    values[WavetableSynth::resonance] = 1.0f;
    const auto swept = play (*makeSynth(), values, 24000, { noteOn (0, 45), noteOn (2400, 52), noteOn (4800, 57) });
    auto peak = 0.0f;

    for (const auto sample : swept.left)
        peak = std::max (peak, std::abs (sample));

    CHECK (std::isfinite (peak));
    CHECK (peak > 0.05f);
    CHECK (peak < 4.0f);
}

TEST_CASE ("The wavetable synth plays 16 voices, glides in mono mode, and plays the same every time")
{
    auto synth = makeSynth();
    auto values = getDefaults();
    std::vector<NoteEvent> chord;

    for (int note = 48; note < 68; ++note)
        chord.push_back (noteOn (0, note));

    play (*synth, values, 4800, chord);
    CHECK (synth->getActiveVoices() == WavetableSynth::maxVoices);

    // Mono: one voice, which glides to the next key held without starting again.
    values = getPlainValues (0.0f);
    values[WavetableSynth::voiceMode] = 1.0f;
    values[WavetableSynth::glide] = 20.0f;
    auto mono = makeSynth();
    const auto glided = play (*mono, values, 48000, { noteOn (0, 57), noteOn (24000, 69) });
    CHECK (mono->getActiveVoices() == 1);
    CHECK (measureFrequency (glided.left, 12000, 24000) == doctest::Approx (220.0).epsilon (0.003));
    CHECK (measureFrequency (glided.left, 38400, 48000) == doctest::Approx (440.0).epsilon (0.003));

    // With unison, noise, a random LFO on the position and random values for each note: the same twice.
    values = getDefaults();
    values[WavetableSynth::unisonVoices] = 6.0f;
    values[WavetableSynth::noiseLevel] = 0.3f;
    values[WavetableSynth::lfo1Shape] = 5.0f; // random
    values[WavetableSynth::mod2Source] = (float) WavetableSynth::ModSource::lfo1;
    values[WavetableSynth::mod2Target] = (float) WavetableSynth::ModTarget::osc1Position;
    values[WavetableSynth::mod2Amount] = 0.5f;
    values[WavetableSynth::mod3Source] = (float) WavetableSynth::ModSource::random;
    values[WavetableSynth::mod3Target] = (float) WavetableSynth::ModTarget::pan;
    values[WavetableSynth::mod3Amount] = 1.0f;
    const std::vector<NoteEvent> notes { noteOn (0, 48), noteOn (3000, 55), noteOff (9000, 48), noteOn (12000, 60) };
    auto first = makeSynth();
    auto second = makeSynth();
    const auto once = play (*first, values, 24000, notes);
    const auto again = play (*second, values, 24000, notes);
    CHECK (once.left == again.left);
    CHECK (once.right == again.right);
}

TEST_CASE ("The wavetable synth lets go of a table it no longer plays once the audio thread can't be reading it")
{
    auto synth = makeSynth();
    auto table = Wavetable::fromHarmonics ("Mine", { makeSaw() });
    const std::weak_ptr<const Wavetable> mine = table;
    synth->setTable (0, std::move (table));
    CHECK (synth->getTable (0) == mine.lock());

    // Swapped for a factory table: the old one stays until two blocks have played, then goes with the next swap.
    synth->setTable (0, devices::getFactoryWavetable (1));
    CHECK (! mine.expired());

    const auto values = getDefaults();
    play (*synth, values, 2 * testBlockSize, { noteOn (0, 60) });
    synth->setTable (0, devices::getFactoryWavetable (2));
    CHECK (mine.expired());

    // Nothing given: the first factory table.
    synth->setTable (1, nullptr);
    CHECK (synth->getTable (1) == devices::getFactoryWavetable (0));
}
