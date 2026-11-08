// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's FM synth (src/devices/fm/FmSynth.h): its description; a carrier alone at its ratio, detuned, or at a fixed
// frequency; a modulator adding sidebands as deep as its level, and more with velocity; the algorithms' carriers and
// modulators, and what each operator modulates; every operator's feedback, toward a saw one way and a square the other,
// without running away or leaving the sound off centre; key scaling and Key Time; phases reset or running free; a new
// synth's electric piano; mono legato with glide; Spread, each note twice, detuned apart, left and right; every algorithm at
// about the same level; the pitch envelope; that it doesn't alias at a high note; what its editor draws; and that it plays
// the same every time.
#include "TestHelpers.h"
#include "devices/fm/FmSynth.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::FmSynth;
using devices::NoteEvent;
using Field = FmSynth::Field;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : FmSynth::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

float& at (std::vector<float>& values, int op, Field field)
{
    return values[(size_t) FmSynth::getParameterIndex (op, field)];
}

// Operator 1 alone, a sine at the note's pitch held at full level: algorithm 1 (a stack), the others off, no velocity,
// nothing that moves; its level at 0 dB.
std::vector<float> getPlainValues()
{
    auto values = getDefaults();
    values[FmSynth::algorithm] = 0.0f;
    values[FmSynth::keyTime] = 0.0f;
    values[FmSynth::outputLevel] = 0.0f;

    for (int op = 0; op < FmSynth::numOperators; ++op)
    {
        at (values, op, Field::on) = op == 0 ? 1.0f : 0.0f;
        at (values, op, Field::ratio) = 1.0f;
        at (values, op, Field::detune) = 0.0f;
        at (values, op, Field::level) = op == 0 ? 1.0f : 0.0f;
        at (values, op, Field::velocity) = 0.0f;
        at (values, op, Field::keyScale) = 0.0f;
        at (values, op, Field::feedback) = 0.0f;
        at (values, op, Field::attack) = 0.5f;
        at (values, op, Field::sustain) = 1.0f;
    }

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

std::unique_ptr<FmSynth> makeSynth()
{
    auto synth = std::make_unique<FmSynth>();
    synth->prepare (testRate, testBlockSize);
    return synth;
}

std::vector<float> play (FmSynth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> played;
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
        block.isPlaying = true;
        synth.process (block);

        for (int i = 0; i < length; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}

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

float peakOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}
} // namespace

TEST_CASE ("The FM synth's description holds together")
{
    REQUIRE (devices::isValid (FmSynth::info));
    CHECK (FmSynth::info.kind == devices::DeviceKind::instrument);
    CHECK (FmSynth::info.name == std::string_view ("FM"));
    CHECK (FmSynth::info.parameters.size() == (size_t) FmSynth::outputLevel + 1);
    CHECK (devices::findParameter (FmSynth::info, "op1Ratio") == FmSynth::getParameterIndex (0, Field::ratio));
    CHECK (devices::findParameter (FmSynth::info, "op4ReleaseCurve") == FmSynth::getParameterIndex (3, Field::releaseCurve));
    CHECK (devices::findParameter (FmSynth::info, "op2Feedback") == FmSynth::getParameterIndex (1, Field::feedback));
    CHECK (devices::findParameter (FmSynth::info, "spread") == FmSynth::spread);
    CHECK (devices::findParameter (FmSynth::info, "pitchAmount") == FmSynth::pitchAmount);
    CHECK (devices::findParameter (FmSynth::info, "level") == FmSynth::outputLevel);

    // Each algorithm: its carriers heard, every operator either heard or modulating, each modulated only by higher ones.
    for (int index = 0; index < FmSynth::numAlgorithms; ++index)
    {
        CAPTURE (index);
        const auto& algorithm = FmSynth::getAlgorithm (index);
        CHECK (algorithm.carriers != 0);

        for (int op = 0; op < FmSynth::numOperators; ++op)
        {
            CHECK ((algorithm.modulatedBy[(size_t) op] & ((1 << (op + 1)) - 1)) == 0);
            auto modulates = false;

            for (int other = 0; other < FmSynth::numOperators; ++other)
                modulates = modulates || ((algorithm.modulatedBy[(size_t) other] >> op) & 1);

            CHECK ((FmSynth::isCarrier (index, op) != modulates));
        }
    }
}

TEST_CASE ("An FM operator plays at its ratio of the note, detuned in cents, or at a fixed frequency")
{
    auto synth = makeSynth();
    auto values = getPlainValues();
    CHECK (measureFrequency (play (*synth, values, 24000, { noteOn (0, 57) }), 2000, 20000) == doctest::Approx (220.0).epsilon (0.002));

    synth->reset();
    at (values, 0, Field::ratio) = 2.0f;
    at (values, 0, Field::fine) = 0.25f;
    CHECK (measureFrequency (play (*synth, values, 24000, { noteOn (0, 57) }), 2000, 20000) == doctest::Approx (495.0).epsilon (0.002));

    synth->reset();
    at (values, 0, Field::ratio) = 1.0f;
    at (values, 0, Field::fine) = 0.0f;
    at (values, 0, Field::detune) = 50.0f;
    CHECK (measureFrequency (play (*synth, values, 24000, { noteOn (0, 57) }), 2000, 20000)
           == doctest::Approx (220.0 * std::pow (2.0, 50.0 / 1200.0)).epsilon (0.002));

    // Fixed: the same frequency whatever the note.
    synth->reset();
    at (values, 0, Field::detune) = 0.0f;
    at (values, 0, Field::fixed) = 1.0f;
    at (values, 0, Field::frequency) = 300.0f;
    CHECK (measureFrequency (play (*synth, values, 24000, { noteOn (0, 40) }), 2000, 20000) == doctest::Approx (300.0).epsilon (0.002));
    synth->reset();
    CHECK (measureFrequency (play (*synth, values, 24000, { noteOn (0, 80) }), 2000, 20000) == doctest::Approx (300.0).epsilon (0.002));
}

TEST_CASE ("An FM modulator adds sidebands as deep as its level, Timbre and velocity, and the carriers decide what is heard")
{
    // Operator 2 at the same ratio, modulating operator 1: harmonics of the note, more of them the deeper it goes.
    const auto secondHarmonic = [] (float level, float timbre, float velocity)
    {
        auto synth = makeSynth();
        auto values = getPlainValues();
        at (values, 1, Field::on) = 1.0f;
        at (values, 1, Field::level) = level;
        at (values, 1, Field::velocity) = 1.0f;
        values[FmSynth::timbre] = timbre;
        const auto played = play (*synth, values, 24000, { noteOn (0, 57, velocity) });
        return measureLevelAt (played, 4000, 20000, 440.0);
    };

    CHECK (secondHarmonic (0.0f, 1.0f, 1.0f) < 0.001);
    CHECK (secondHarmonic (0.3f, 1.0f, 1.0f) > 0.02);
    CHECK (secondHarmonic (0.5f, 1.0f, 1.0f) > secondHarmonic (0.3f, 1.0f, 1.0f));
    CHECK (secondHarmonic (0.5f, 0.0f, 1.0f) < 0.001);                                   // Timbre at none takes the modulator away
    CHECK (secondHarmonic (0.5f, 1.0f, 0.3f) < secondHarmonic (0.5f, 1.0f, 1.0f) * 0.6); // softer, duller

    // In a stack, only operator 1 is heard: off, nothing; in four side by side, each is heard.
    auto synth = makeSynth();
    auto values = getPlainValues();
    at (values, 0, Field::on) = 0.0f;
    at (values, 1, Field::on) = 1.0f;
    at (values, 1, Field::level) = 1.0f;
    CHECK (peakOf (play (*synth, values, 4800, { noteOn (0, 57) }), 0, 4800) == 0.0f);

    synth->reset();
    values[FmSynth::algorithm] = 7.0f;
    CHECK (measureFrequency (play (*synth, values, 24000, { noteOn (0, 57) }), 2000, 20000) == doctest::Approx (220.0).epsilon (0.002));
}

TEST_CASE ("What each FM operator modulates in each algorithm")
{
    CHECK (FmSynth::getTargets (0, 3) == 0b0100); // a stack: 4 into 3, 3 into 2, 2 into 1
    CHECK (FmSynth::getTargets (0, 1) == 0b0001);
    CHECK (FmSynth::getTargets (0, 0) == 0);      // a carrier modulates nothing
    CHECK (FmSynth::getTargets (4, 3) == 0b0100); // two pairs: 4 into 3, 2 into 1
    CHECK (FmSynth::getTargets (4, 1) == 0b0001);
    CHECK (FmSynth::getTargets (6, 3) == 0b0111); // one into three
    CHECK (FmSynth::getTargets (7, 3) == 0);      // four side by side
}

TEST_CASE ("Every FM operator's feedback brightens it toward a saw one way and a square the other, without running away")
{
    // Operator 4 heard alone (four side by side, the rest off), or operator 1 heard alone in a stack: feedback works on
    // every operator, heard or not.
    struct Heard
    {
        double second, third, mean;
        float peak;
    };

    const auto play110 = [] (int op, float feedback, float timbre = 1.0f)
    {
        auto synth = makeSynth();
        auto values = getPlainValues();
        values[FmSynth::algorithm] = op == 3 ? 7.0f : 0.0f;
        values[FmSynth::timbre] = timbre;

        for (int each = 0; each < FmSynth::numOperators; ++each)
            at (values, each, Field::on) = each == op ? 1.0f : 0.0f;

        at (values, op, Field::level) = 1.0f;
        at (values, op, Field::feedback) = feedback;
        const auto played = play (*synth, values, 48000, { noteOn (0, 45) }); // 110 Hz
        const auto fundamental = measureLevelAt (played, 8000, 44000, 110.0);
        auto sum = 0.0;

        for (size_t i = 24000; i < 48000; ++i)
            sum += played[i];

        return Heard { measureLevelAt (played, 8000, 44000, 220.0) / fundamental, measureLevelAt (played, 8000, 44000, 330.0) / fundamental,
                       sum / 24000.0, peakOf (played, 0, played.size()) };
    };

    for (const auto op : { 3, 0 })
    {
        CAPTURE (op);
        CHECK (play110 (op, 0.0f).second < 0.01);
        CHECK (play110 (op, 0.0f).third < 0.01);

        // Toward a saw: every harmonic, more of them the deeper it goes, up to half (the DX7's feedback 6).
        CHECK (play110 (op, 0.25f).second > 0.1);
        CHECK (play110 (op, 0.5f).second > play110 (op, 0.25f).second);

        // Toward a square: odd harmonics only, up to half.
        CHECK (play110 (op, -0.25f).third > 0.1);
        CHECK (play110 (op, -0.5f).third > play110 (op, -0.25f).third);
        CHECK (play110 (op, -0.5f).second < 0.01);

        // At full (the DX7's feedback 7), rough, bounded (its DC blocker can take a peak past the sine's, at most twice it,
        // as what heavy feedback makes below hearing goes), and centred.
        CHECK (play110 (op, 0.5f).peak < 1.0f);
        CHECK (play110 (op, 1.0f).peak < 1.5f);
        CHECK (play110 (op, -1.0f).peak < 1.5f);
        CHECK (std::abs (play110 (op, 1.0f).mean) < 0.002);
        CHECK (std::abs (play110 (op, -1.0f).mean) < 0.002);
    }

    // Timbre turns feedback down with the modulators: at none, a sine again.
    CHECK (play110 (3, 1.0f, 0.0f).second < 0.01);
}

TEST_CASE ("The FM synth stays centred: what a modulator a few cents off its carrier's ratio leaves below hearing is taken away")
{
    // A modulator at the carrier's ratio makes a sideband at 0 Hz; a few cents apart, it wanders slowly up and down.
    auto synth = makeSynth();
    auto values = getPlainValues();
    at (values, 1, Field::on) = 1.0f;
    at (values, 1, Field::level) = 0.6f;
    at (values, 1, Field::detune) = 8.0f;
    at (values, 1, Field::feedback) = 0.25f;
    const auto played = play (*synth, values, 4 * 48000, { noteOn (0, 45) });

    for (size_t from = 48000; from + 9600 <= played.size(); from += 9600)
    {
        CAPTURE (from);
        auto sum = 0.0;

        for (auto i = from; i < from + 9600; ++i)
            sum += played[i];

        CHECK (std::abs (sum / 9600.0) < 0.01);
    }
}

TEST_CASE ("FM Spread plays each note twice, detuned apart, the lower on the left and the higher on the right")
{
    const auto playBoth = [] (const std::vector<float>& values, int note)
    {
        auto synth = makeSynth();
        juce::AudioBuffer<float> buffer (2, testBlockSize);
        std::array<std::vector<float>, 2> played;

        for (int done = 0; done < 48000; done += testBlockSize)
        {
            buffer.clear();
            std::vector<NoteEvent> events;

            if (done == 0)
                events.push_back (noteOn (0, note));

            float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = testBlockSize;
            block.parameters = values;
            block.events = events;
            block.isPlaying = true;
            synth->process (block);

            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < testBlockSize; ++i)
                    played[(size_t) channel].push_back (buffer.getSample (channel, i));
        }

        return played;
    };

    // Off: one sound in the middle.
    auto values = getPlainValues();
    const auto centred = playBoth (values, 57);
    CHECK (centred[0] == centred[1]);

    // At 20 cents: each side 10 cents off the note, as loud as the one in the middle was.
    values[FmSynth::spread] = 20.0f;
    const auto spread = playBoth (values, 57);
    CHECK (spread[0] != spread[1]);
    CHECK (measureFrequency (spread[0], 2000, 46000) == doctest::Approx (220.0 * std::pow (2.0, -10.0 / 1200.0)).epsilon (0.0005));
    CHECK (measureFrequency (spread[1], 2000, 46000) == doctest::Approx (220.0 * std::pow (2.0, 10.0 / 1200.0)).epsilon (0.0005));
    CHECK (peakOf (spread[0], 4000, 46000) == doctest::Approx (peakOf (centred[0], 4000, 46000)).epsilon (0.02));

    // In mono mode too.
    values[FmSynth::voiceMode] = 1.0f;
    const auto mono = playBoth (values, 57);
    CHECK (mono[0] != mono[1]);
    CHECK (measureFrequency (mono[1], 2000, 46000) == doctest::Approx (220.0 * std::pow (2.0, 10.0 / 1200.0)).epsilon (0.0005));
}

TEST_CASE ("What the FM synth's editor draws of its sound: each operator as it plays and the carriers' sum")
{
    auto drawing = std::make_unique<FmSynth::Drawing>();
    const std::array<float, FmSynth::numOperators> peaks { 1.0f, 1.0f, 1.0f, 1.0f };

    // Operator 1 alone: a sine, two cycles, at its level as heard, a square root of one carrier down with the headroom.
    auto values = getPlainValues();
    FmSynth::draw (values, peaks, *drawing);
    const auto peak = *std::max_element (drawing->heard.begin(), drawing->heard.end());
    CHECK (peak == doctest::Approx (0.5623f).epsilon (0.01));

    for (int n = 0; n < FmSynth::drawnLength; n += 37)
    {
        CAPTURE (n);
        CHECK (drawing->heard[(size_t) n]
               == doctest::Approx (peak * std::sin (twoPi * n / FmSynth::drawnPerCycle)).epsilon (0.01).scale (1.0));
    }

    // Operator 2 at three times the pitch, bending it: six cycles of its own over the two, and operator 1 no longer a sine.
    const auto crossings = [] (const std::array<float, FmSynth::drawnLength>& wave)
    {
        auto count = 0;

        for (size_t n = 1; n < wave.size(); ++n)
            count += (wave[n - 1] < 0.0f) != (wave[n] < 0.0f) ? 1 : 0;

        return count;
    };

    at (values, 1, Field::on) = 1.0f;
    at (values, 1, Field::level) = 0.5f;
    at (values, 1, Field::ratio) = 3.0f;
    FmSynth::draw (values, peaks, *drawing);
    CHECK (std::abs (crossings (drawing->operators[1]) - 12) <= 1);
    auto bent = 0.0f;

    for (int n = 0; n < FmSynth::drawnLength; ++n)
        bent = std::max (bent, std::abs (drawing->heard[(size_t) n] - peak * (float) std::sin (twoPi * n / FmSynth::drawnPerCycle)));

    CHECK (bent > 0.1f);

    // Its envelope at none: a sine again.
    FmSynth::draw (values, std::array<float, FmSynth::numOperators> { 1.0f, 0.0f, 1.0f, 1.0f }, *drawing);
    CHECK (drawing->heard[128] == doctest::Approx (peak).epsilon (0.01));
}

TEST_CASE ("Key scaling turns a modulator down up the keys, and Key Time makes envelopes shorter there")
{
    // Operator 2's key scale at full: an octave up, half as deep.
    const auto depthAt = [] (int note, float keyScale)
    {
        auto synth = makeSynth();
        auto values = getPlainValues();
        at (values, 1, Field::on) = 1.0f;
        at (values, 1, Field::level) = 0.3f;
        at (values, 1, Field::keyScale) = keyScale;
        const auto played = play (*synth, values, 24000, { noteOn (0, note) });
        return measureLevelAt (played, 4000, 20000, 2.0 * noteFrequency (note))
               / measureLevelAt (played, 4000, 20000, noteFrequency (note));
    };

    CHECK (depthAt (72, 1.0f) < depthAt (60, 1.0f) * 0.7);
    CHECK (depthAt (72, 0.0f) == doctest::Approx (depthAt (60, 0.0f)).epsilon (0.15));

    // Key Time at full: a note an octave up decays in half the time.
    const auto decayed = [] (int note, float keyTime)
    {
        auto synth = makeSynth();
        auto values = getPlainValues();
        values[FmSynth::keyTime] = keyTime;
        at (values, 0, Field::decay) = 400.0f;
        at (values, 0, Field::sustain) = 0.0f;
        const auto played = play (*synth, values, 24000, { noteOn (0, note) });
        return peakOf (played, 12000, 12600); // a quarter second in
    };

    CHECK (decayed (84, 1.0f) < decayed (60, 1.0f) * 0.5f);
}

TEST_CASE ("The FM synth's phases start each note the same with Reset, and run free otherwise")
{
    auto values = getPlainValues();
    at (values, 1, Field::on) = 1.0f;
    at (values, 1, Field::level) = 0.4f;
    at (values, 1, Field::ratio) = 3.0f;

    auto again = makeSynth();
    CHECK (play (*again, values, 4800, { noteOn (0, 57) }) == play (*makeSynth(), values, 4800, { noteOn (0, 57) }));

    values[FmSynth::phase] = 1.0f; // free
    auto running = makeSynth();
    const auto one = play (*running, values, 2400, { noteOn (0, 57) });
    running->reset();
    const auto two = play (*running, values, 2400, { noteOn (0, 57) });
    CHECK (one != two);
}

TEST_CASE ("A new FM synth plays a soft electric piano that fades, brighter the harder it is played")
{
    auto synth = makeSynth();
    const auto values = getDefaults();
    const auto played = play (*synth, values, (int) (6.0 * testRate), { noteOn (0, 60, 0.9f) });
    CHECK (peakOf (played, 0, 4800) > 0.05f);
    CHECK (peakOf (played, 0, played.size()) < 1.0f);
    CHECK (peakOf (played, (size_t) (5.0 * testRate), played.size()) < peakOf (played, 0, 4800) * 0.05f); // nothing sustains

    const auto brightAt = [] (float velocity)
    {
        auto piano = makeSynth();
        const auto sound = play (*piano, getDefaults(), 9600, { noteOn (0, 60, velocity) });
        return measureLevelAt (sound, 200, 4000, 14.0 * noteFrequency (60)) / measureLevelAt (sound, 200, 4000, noteFrequency (60));
    };

    CHECK (brightAt (1.0f) > brightAt (0.3f) * 1.5);
}

TEST_CASE ("The FM synth's mono mode glides between held keys without starting again")
{
    auto synth = makeSynth();
    auto values = getPlainValues();
    values[FmSynth::voiceMode] = 1.0f;
    values[FmSynth::glide] = 100.0f;
    const auto played = play (*synth, values, (int) testRate, { noteOn (0, 57), noteOn (12000, 64) });
    CHECK (synth->getActiveVoices() == 1);
    CHECK (measureFrequency (played, 2000, 11000) == doctest::Approx (220.0).epsilon (0.003));
    CHECK (measureFrequency (played, 36000, 47000) == doctest::Approx (noteFrequency (64)).epsilon (0.004));
}

TEST_CASE ("Every FM algorithm plays at about the same level: its carriers' sum is a square root of their number down")
{
    // Every operator on at full level, each at its own ratio, and Timbre at none, so that only the carriers are heard: one
    // sine in a stack, four side by side.
    auto values = getPlainValues();
    values[FmSynth::timbre] = 0.0f;

    for (int op = 0; op < FmSynth::numOperators; ++op)
    {
        at (values, op, Field::on) = 1.0f;
        at (values, op, Field::level) = 1.0f;
        at (values, op, Field::ratio) = (float) (op + 1);
    }

    std::vector<double> levels;

    for (int algorithm = 0; algorithm < FmSynth::numAlgorithms; ++algorithm)
    {
        values[FmSynth::algorithm] = (float) algorithm;
        auto synth = makeSynth();
        const auto played = play (*synth, values, 24000, { noteOn (0, 57) });
        auto sum = 0.0;

        for (size_t i = 4000; i < 20000; ++i)
            sum += (double) played[i] * played[i];

        levels.push_back (std::sqrt (sum / 16000.0));
    }

    const auto [quietest, loudest] = std::minmax_element (levels.begin(), levels.end());
    CHECK (*quietest > 0.1);
    CHECK (20.0 * std::log10 (*loudest / *quietest) < 0.5);
}

TEST_CASE ("The FM synth's pitch envelope starts each note above its pitch and falls onto it over its decay")
{
    auto synth = makeSynth();
    auto values = getPlainValues();
    values[FmSynth::pitchAmount] = 12.0f;
    values[FmSynth::pitchDecay] = 30.0f;
    const auto played = play (*synth, values, 24000, { noteOn (0, 57) });
    const auto start = measureFrequency (played, 0, 480), later = measureFrequency (played, 1440, 2880);
    CHECK (start > 1.5 * 220.0); // most of an octave up in its first 10 ms
    CHECK (later < start);       // falling
    CHECK (later > 220.0 * 1.02);
    CHECK (measureFrequency (played, 12000, 20000) == doctest::Approx (220.0).epsilon (0.002)); // then on its pitch
}

TEST_CASE ("The FM synth doesn't fold a high note's sidebands back down as aliasing")
{
    // C7 (2093 Hz), a modulator at the same ratio at full depth: its sidebands reach past half the rate. What folds back
    // would land between the note's harmonics; at twice the rate, little does.
    auto synth = makeSynth();
    auto values = getPlainValues();
    at (values, 1, Field::on) = 1.0f;
    at (values, 1, Field::level) = 0.6f;
    const auto played = play (*synth, values, 48000, { noteOn (0, 96) });
    const auto f0 = noteFrequency (96);
    const auto fundamental = measureLevelAt (played, 4000, 44000, f0);
    auto worst = 0.0;

    for (const auto between : { 0.5, 1.5, 2.5, 3.5 })
        worst = std::max (worst, measureLevelAt (played, 4000, 44000, between * f0));

    CHECK (worst < fundamental * 0.03); // more than 30 dB down
}

TEST_CASE ("The FM synth plays the same every time, and lets every note go")
{
    const auto values = getDefaults();
    const std::vector<NoteEvent> chords { noteOn (0, 60),      noteOn (0, 64),      noteOn (0, 67),
                                          noteOff (24000, 60), noteOff (24000, 64), noteOff (24000, 67) };
    auto first = makeSynth();
    auto second = makeSynth();
    CHECK (play (*first, values, 48000, chords) == play (*second, values, 48000, chords));

    play (*first, values, 4 * 48000, {});
    CHECK (first->getActiveVoices() == 0);
}
