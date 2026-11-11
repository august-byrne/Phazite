// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's FM synth (src/devices/fm/FmSynth.h): its description; a carrier alone at its ratio, detuned, or at a fixed
// frequency; a modulator adding sidebands as deep as its level, and more with velocity; the algorithms' carriers and
// modulators, and what each operator modulates; every operator's feedback, toward a saw one way and a square the other,
// without running away or leaving the sound off centre; key scaling and Key Time; phases reset or running free, and each
// operator's own place in its wave as a note starts; a new synth's electric piano; mono legato with glide; a key played
// again, as it rings out, while it is held or under the pedal, each note on a voice of its own; Spread, each note twice,
// detuned apart, left and right; every algorithm at about the same level; the pitch envelope; its filter's envelope and
// velocity, which move its cutoff as the synth's move its own; that it doesn't alias at a high note; what its editor draws;
// and that it plays the same every time.
#include "TestHelpers.h"
#include "devices/fm/FmSynth.h"
#include "devices/synth/Synth.h"

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

std::unique_ptr<FmSynth> makeSynth (double rate = testRate)
{
    auto synth = std::make_unique<FmSynth>();
    synth->prepare (rate, testBlockSize);
    return synth;
}

// Plays the synth in blocks, with events at their samples from the start, and returns its left channel; after each block,
// afterBlock is called.
template <typename AfterBlock>
std::vector<float> play (devices::Processor& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
                         AfterBlock afterBlock)
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
        afterBlock();

        for (int i = 0; i < length; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}

std::vector<float> play (devices::Processor& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events)
{
    return play (synth, values, numSamples, events, [] {});
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

// Where a frequency is in its cycle over a stretch of sound, in degrees, as a sine's phase at the stretch's start.
double measurePhaseAt (const std::vector<float>& sound, size_t from, size_t to, double frequency)
{
    auto inPhase = 0.0, quadrature = 0.0;

    for (auto i = from; i < to; ++i)
    {
        const auto angle = twoPi * frequency * (double) (i - from) / testRate;
        inPhase += sound[i] * std::sin (angle);
        quadrature += sound[i] * std::cos (angle);
    }

    return juce::radiansToDegrees (std::atan2 (quadrature, inPhase));
}

// Operator 1 alone, a saw, bright, through the filter: a low-pass at 500 Hz that neither the key nor the resonance moves,
// its envelope straight from full (no attack) down to half over 100 ms, back to nothing over 50 ms.
std::vector<float> getFilteredValues()
{
    auto values = getPlainValues();
    at (values, 0, Field::wave) = (float) FmSynth::Wave::saw;
    at (values, 0, Field::release) = 1000.0f;
    values[FmSynth::filterOn] = 1.0f;
    values[FmSynth::cutoff] = 500.0f;
    values[FmSynth::resonance] = 0.0f;
    values[FmSynth::keyTracking] = 0.0f;
    values[FmSynth::filterAttack] = 0.0f;
    values[FmSynth::filterDecay] = 100.0f;
    values[FmSynth::filterSustain] = 0.5f;
    values[FmSynth::filterRelease] = 50.0f;
    values[FmSynth::filterAttackCurve] = values[FmSynth::filterDecayCurve] = values[FmSynth::filterReleaseCurve] = 0.0f;
    return values;
}

// The cutoff the newest note plays once a synth has played some samples of a note.
float cutoffAfter (FmSynth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events)
{
    play (synth, values, numSamples, events);
    return synth.getShown().cutoff.get();
}

// The largest difference between two sounds, from a sample on.
float largestDifference (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0)
{
    auto largest = 0.0f;

    for (auto i = from; i < a.size() && i < b.size(); ++i)
        largest = std::max (largest, std::abs (a[i] - b[i]));

    return largest;
}

NoteEvent pedal (int sample, bool down)
{
    return { NoteEvent::Type::controller, sample, 64, down ? 1.0f : 0.0f };
}

// What a new synth plays of some events, with the voices sounding after each block, and what new synths play of each group
// of them alone, added: a key played again plays each of its notes as it would alone, what rings out of the one before
// untouched and the new one from its start, as a note on a voice of its own does. The tests' events are on the synth's
// control grid (16 samples), as its blocks are, so that each note's voice moves at the same samples both ways.
struct Apart
{
    std::vector<float> together, added;
    std::vector<int> voicesAfter;
};

Apart playApart (const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& together,
                 const std::vector<std::vector<NoteEvent>>& groups)
{
    Apart played;
    const auto synth = makeSynth();
    played.together = play (*synth, values, numSamples, together, [&] { played.voicesAfter.push_back (synth->getActiveVoices()); });
    played.added.assign (played.together.size(), 0.0f);

    for (const auto& group : groups)
    {
        const auto part = play (*makeSynth(), values, numSamples, group);

        for (size_t i = 0; i < part.size(); ++i)
            played.added[i] += part[i];
    }

    return played;
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
    CHECK (devices::findParameter (FmSynth::info, "op3Phase") == FmSynth::getParameterIndex (2, Field::startPhase));
    CHECK (devices::findParameter (FmSynth::info, "filterEnvelope") == FmSynth::filterEnvelope);
    CHECK (devices::findParameter (FmSynth::info, "filterReleaseCurve") == FmSynth::filterReleaseCurve);
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

TEST_CASE ("An FM operator starts each note where its Phase says, in degrees, while the phases reset")
{
    // Operator 1 alone, a sine at 220 Hz: half a turn in, the same wave upside down; a quarter in, a quarter of a cycle
    // ahead; a whole turn in, the start again.
    const auto playFrom = [] (float degrees)
    {
        auto synth = makeSynth();
        auto values = getPlainValues();
        at (values, 0, Field::startPhase) = degrees;
        return play (*synth, values, 9600, { noteOn (0, 57) });
    };

    const auto start = playFrom (0.0f), quarter = playFrom (90.0f), half = playFrom (180.0f);
    REQUIRE (peakOf (start, 0, start.size()) > 0.4f);

    for (size_t i = 0; i < start.size(); i += 7)
        CHECK (half[i] == doctest::Approx (-start[i]).epsilon (0.001).scale (1.0));

    auto ahead = measurePhaseAt (quarter, 2400, 9600, 220.0) - measurePhaseAt (start, 2400, 9600, 220.0);
    ahead -= 360.0 * std::floor (ahead / 360.0);
    CHECK (ahead == doctest::Approx (90.0).epsilon (0.01));
    CHECK (playFrom (360.0f) == start);

    // It is in the description as an angle, from none (as every sound was before it had one) to a whole turn.
    const auto& info = FmSynth::info.parameters[(size_t) FmSynth::getParameterIndex (1, Field::startPhase)];
    CHECK (info.name == std::string_view ("Op 2 Phase"));
    CHECK (info.unit == devices::Unit::degrees);
    CHECK (info.minimum == 0.0f);
    CHECK (info.maximum == 360.0f);
    CHECK (info.defaultValue == 0.0f);
}

TEST_CASE ("Where an FM modulator starts against its carrier changes the tone, by as much as the Bessel functions say")
{
    // A sine bent by a sine at its own pitch, sin (x + I sin (x + p)): its second harmonic is J1 (I) + J3 (I) with the two
    // in phase (p = 0) and J1 (I) - J3 (I) with the modulator a quarter of a cycle ahead. At a level of 0.4, I = 2.0106:
    // J1 = 0.57602 and J3 = 0.13064.
    const auto secondHarmonic = [] (float carrierDegrees, float modulatorDegrees, FmSynth::Phase phase = FmSynth::Phase::reset)
    {
        auto synth = makeSynth();
        auto values = getPlainValues();
        at (values, 1, Field::on) = 1.0f;
        at (values, 1, Field::level) = 0.4f;
        at (values, 0, Field::startPhase) = carrierDegrees;
        at (values, 1, Field::startPhase) = modulatorDegrees;
        values[FmSynth::phase] = (float) phase;
        const auto played = play (*synth, values, 24000, { noteOn (0, 57) });
        return measureLevelAt (played, 4000, 20000, 440.0);
    };

    constexpr double j1 = 0.57602, j3 = 0.13064, carrierGain = 0.56234133;
    CHECK (secondHarmonic (0.0f, 0.0f) == doctest::Approx (carrierGain * (j1 + j3)).epsilon (0.01));
    CHECK (secondHarmonic (0.0f, 90.0f) == doctest::Approx (carrierGain * (j1 - j3)).epsilon (0.01));

    // Only where each starts against the other counts: both a quarter in is both at the start.
    CHECK (secondHarmonic (90.0f, 90.0f) == doctest::Approx (secondHarmonic (0.0f, 0.0f)).epsilon (0.001));
    CHECK (secondHarmonic (270.0f, 0.0f) == doctest::Approx (secondHarmonic (0.0f, 90.0f)).epsilon (0.001));

    // While the phases run free, the operators go on from where they were, and their own places mean nothing.
    CHECK (secondHarmonic (0.0f, 90.0f, FmSynth::Phase::free) == secondHarmonic (0.0f, 0.0f, FmSynth::Phase::free));
}

TEST_CASE ("The FM synth's editor's picture starts each operator where its Phase says")
{
    auto drawing = std::make_unique<FmSynth::Drawing>();
    const std::array<float, FmSynth::numOperators> peaks { 1.0f, 1.0f, 1.0f, 1.0f };
    auto values = getPlainValues();
    at (values, 0, Field::startPhase) = 90.0f;
    FmSynth::draw (values, peaks, *drawing);

    for (int n = 0; n < FmSynth::drawnLength; n += 37)
    {
        CAPTURE (n);
        CHECK (drawing->heard[(size_t) n]
               == doctest::Approx (0.5623 * std::cos (twoPi * n / FmSynth::drawnPerCycle)).epsilon (0.01).scale (1.0));
    }

    // Running free, from the wave's start, as before.
    values[FmSynth::phase] = (float) FmSynth::Phase::free;
    FmSynth::draw (values, peaks, *drawing);
    CHECK (drawing->heard[128] == doctest::Approx (0.5623f).epsilon (0.01));
}

TEST_CASE ("The FM synth's filter envelope opens the cutoff by its amount in semitones, and closes it as its stages say")
{
    // No attack, so it starts full: the cutoff two octaves up at once, which lets a saw's high harmonics through.
    auto values = getFilteredValues();
    values[FmSynth::filterEnvelope] = 24.0f;
    values[FmSynth::filterDecay] = 10000.0f;
    values[FmSynth::filterSustain] = 1.0f;
    auto synth = makeSynth();
    CHECK (cutoffAfter (*synth, values, testBlockSize, { noteOn (0, 45) }) == doctest::Approx (2000.0f).epsilon (0.001));

    values[FmSynth::filterEnvelope] = -12.0f; // closing it instead
    synth = makeSynth();
    CHECK (cutoffAfter (*synth, values, testBlockSize, { noteOn (0, 45) }) == doctest::Approx (250.0f).epsilon (0.001));

    values[FmSynth::filterEnvelope] = 48.0f; // four octaves over 5 kHz: as high as the rate can play
    values[FmSynth::cutoff] = 5000.0f;
    synth = makeSynth();
    CHECK (cutoffAfter (*synth, values, testBlockSize, { noteOn (0, 45) }) == doctest::Approx (0.45f * (float) testRate));

    // Falling straight to half over 100 ms: where it has got to as each block's last step starts, 16 samples before its end.
    values = getFilteredValues();
    values[FmSynth::filterEnvelope] = 24.0f;
    synth = makeSynth();
    const auto levelAt = [] (int sample) { return 1.0 - 0.5 * std::min (1.0, sample / (0.1 * testRate)); };
    auto played = 0;

    for (int block = 0; block < 24; ++block)
    {
        CAPTURE (block);
        const auto cutoffNow =
            cutoffAfter (*synth, values, testBlockSize, block == 0 ? std::vector<NoteEvent> { noteOn (0, 45) } : std::vector<NoteEvent> {});
        played += testBlockSize;
        CHECK (cutoffNow == doctest::Approx (500.0 * std::pow (2.0, 2.0 * levelAt (played - 16))).epsilon (0.002));
    }

    // Held at its sustain, an octave up; let go, back to where the cutoff is set within its release, while the note rings on.
    CHECK (synth->getShown().cutoff.get() == doctest::Approx (1000.0f).epsilon (0.002));
    CHECK (synth->getShown().filterEnvelope.get() == doctest::Approx (0.5f).epsilon (0.002));
    CHECK (cutoffAfter (*synth, values, (int) (0.06 * testRate), { noteOff (0, 45) }) == doctest::Approx (500.0f).epsilon (0.002));
    CHECK (synth->getActiveVoices() == 1);

    // Heard: the saw's twentieth harmonic, two octaves over the cutoff, is far louder while the envelope holds the filter open
    // than without an envelope.
    const auto twentieth = [] (float amount)
    {
        auto held = getFilteredValues();
        held[FmSynth::filterEnvelope] = amount;
        held[FmSynth::filterSustain] = 1.0f;
        const auto sound = play (*makeSynth(), held, 24000, { noteOn (0, 45) });
        return measureLevelAt (sound, 4000, 20000, 2200.0);
    };

    CHECK (twentieth (36.0f) > 30.0 * twentieth (0.0f));
}

TEST_CASE ("The FM synth's Filter Velocity closes the cutoff for softer notes: at full, four octaves at velocity 0")
{
    auto values = getFilteredValues();
    const auto cutoffAt = [&values] (float sensitivity, float velocity)
    {
        values[FmSynth::filterVelocity] = sensitivity;
        auto synth = makeSynth();
        return cutoffAfter (*synth, values, testBlockSize, { noteOn (0, 45, velocity) });
    };

    values[FmSynth::cutoff] = 4000.0f;
    CHECK (cutoffAt (0.0f, 0.5f) == doctest::Approx (4000.0f).epsilon (0.001));
    CHECK (cutoffAt (1.0f, 1.0f) == doctest::Approx (4000.0f).epsilon (0.001)); // a note at full velocity, where it is set
    CHECK (cutoffAt (0.5f, 0.5f) == doctest::Approx (2000.0f).epsilon (0.001)); // half of 48 semitones, half way down: an octave
    CHECK (cutoffAt (1.0f, 0.5f) == doctest::Approx (1000.0f).epsilon (0.001));
    CHECK (cutoffAt (1.0f, 0.0f) == doctest::Approx (250.0f).epsilon (0.001));
}

TEST_CASE ("The FM synth's filter envelope and velocity are the synth's: the same names, ranges and units, and the same cutoff played")
{
    using devices::Synth;

    // Described alike, but for the envelope's amount, none in a new FM synth (its filter is off, and stays where it is set
    // once on) and two octaves in a new synth.
    for (const auto* id : { "filterEnvelope", "filterVelocity", "filterAttack", "filterDecay", "filterSustain", "filterRelease",
                            "filterAttackCurve", "filterDecayCurve", "filterReleaseCurve", "cutoff", "keyTracking" })
    {
        CAPTURE (id);
        const auto fmIndex = devices::findParameter (FmSynth::info, id), synthIndex = devices::findParameter (Synth::info, id);
        REQUIRE (fmIndex >= 0);
        REQUIRE (synthIndex >= 0);
        const auto& ours = FmSynth::info.parameters[(size_t) fmIndex];
        const auto& theirs = Synth::info.parameters[(size_t) synthIndex];
        CHECK (ours.name == theirs.name);
        CHECK (ours.minimum == theirs.minimum);
        CHECK (ours.maximum == theirs.maximum);
        CHECK (ours.unit == theirs.unit);
        CHECK (ours.midpoint == theirs.midpoint);

        if (std::string_view (id) != "filterEnvelope" && std::string_view (id) != "cutoff")
            CHECK (ours.defaultValue == theirs.defaultValue);
    }

    CHECK (FmSynth::info.parameters[FmSynth::filterEnvelope].defaultValue == 0.0f);

    // Played alike: each with the same filter settings, a note at velocity 0.6 held, then let go; the cutoff each plays,
    // block by block.
    auto fmValues = getFilteredValues();
    std::vector<float> synthValues;

    for (const auto& parameter : Synth::info.parameters)
        synthValues.push_back (parameter.defaultValue);

    const auto set = [&fmValues, &synthValues] (const char* id, float value)
    {
        fmValues[(size_t) devices::findParameter (FmSynth::info, id)] = value;
        synthValues[(size_t) devices::findParameter (Synth::info, id)] = value;
    };

    set ("cutoff", 700.0f);
    set ("keyTracking", 0.3f);
    set ("filterEnvelope", 30.0f);
    set ("filterVelocity", 0.4f);
    set ("filterAttack", 0.0f);
    set ("filterDecay", 80.0f);
    set ("filterSustain", 0.3f);
    set ("filterRelease", 40.0f);
    set ("filterAttackCurve", 0.2f);
    set ("filterDecayCurve", -0.3f);
    set ("filterReleaseCurve", -0.1f);
    synthValues[Synth::release] = 1000.0f;

    auto fm = makeSynth();
    Synth synth;
    synth.prepare (testRate, testBlockSize);

    for (int block = 0; block < 40; ++block)
    {
        CAPTURE (block);
        const auto events = block == 0    ? std::vector<NoteEvent> { noteOn (0, 67, 0.6f) }
                            : block == 30 ? std::vector<NoteEvent> { noteOff (0, 67) }
                                          : std::vector<NoteEvent> {};
        play (*fm, fmValues, testBlockSize, events);
        play (synth, synthValues, testBlockSize, events);
        CHECK (fm->getShown().cutoff.get() == doctest::Approx (synth.getShown().cutoff.get()).epsilon (0.0001));
    }

    CHECK (fm->getShown().cutoff.get() < 800.0f); // back down, both
}

TEST_CASE ("With no envelope amount and no Filter Velocity, the FM synth's filter plays as before, whatever its envelope is set to")
{
    // A new synth's piano through its filter, soft notes and hard: nothing the envelope's stages are set to changes a sample.
    auto values = getDefaults();
    values[FmSynth::filterOn] = 1.0f;
    const std::vector<NoteEvent> notes { noteOn (0, 48, 0.4f), noteOn (3000, 67, 1.0f), noteOff (20000, 48), noteOff (20000, 67) };
    const auto before = play (*makeSynth(), values, 30000, notes);
    REQUIRE (peakOf (before, 0, before.size()) > 0.05f);

    values[FmSynth::filterAttack] = 300.0f;
    values[FmSynth::filterDecay] = 20.0f;
    values[FmSynth::filterSustain] = 1.0f;
    values[FmSynth::filterRelease] = 5.0f;
    values[FmSynth::filterDecayCurve] = 1.0f;
    CHECK (play (*makeSynth(), values, 30000, notes) == before);

    // And every operator from its wave's start, the angle a new synth has.
    for (int op = 0; op < FmSynth::numOperators; ++op)
        CHECK (at (values, op, Field::startPhase) == 0.0f);
}

TEST_CASE ("Key Time shortens the FM synth's filter envelope up the keys with its operators' envelopes")
{
    // At full, an octave up, half the time: the cutoff a note at C5 plays 24 ms in is the one a note at C4 plays 48 ms in.
    auto values = getFilteredValues();
    values[FmSynth::filterEnvelope] = 24.0f;
    values[FmSynth::filterDecay] = 200.0f;
    values[FmSynth::filterSustain] = 0.0f;
    values[FmSynth::keyTime] = 1.0f;
    auto low = makeSynth(), high = makeSynth();
    const auto lowCutoff = cutoffAfter (*low, values, 18 * testBlockSize, { noteOn (0, 60) });
    const auto highCutoff = cutoffAfter (*high, values, 9 * testBlockSize, { noteOn (0, 72) });
    CHECK (lowCutoff < 1100.0f); // about half way down its two octaves
    CHECK (lowCutoff > 950.0f);
    CHECK (highCutoff == doctest::Approx (lowCutoff).epsilon (0.01));

    values[FmSynth::keyTime] = 0.0f; // none: the same time at every key
    high = makeSynth();
    CHECK (cutoffAfter (*high, values, 18 * testBlockSize, { noteOn (0, 72) }) == doctest::Approx (lowCutoff).epsilon (0.001));
}

TEST_CASE ("A voice's next note starts the FM synth's filter envelope from nothing, though its last release was cut short")
{
    // The filter's release far longer than the note's: the voice ends with its filter still closing, and its next note plays
    // as a new synth's first does.
    auto values = getFilteredValues();
    values[FmSynth::filterEnvelope] = 36.0f;
    values[FmSynth::filterAttack] = 20.0f;
    values[FmSynth::filterRelease] = 5000.0f;
    at (values, 0, Field::release) = 20.0f;
    auto synth = makeSynth();
    play (*synth, values, 9600, { noteOn (0, 45), noteOff (4800, 45) });
    REQUIRE (synth->getActiveVoices() == 0);
    CHECK (play (*synth, values, 4800, { noteOn (0, 45) }) == play (*makeSynth(), values, 4800, { noteOn (0, 45) }));
}

TEST_CASE ("The FM synth's filter envelope plays the same at every sample rate, and a fast sweep at full resonance stays bounded")
{
    auto values = getFilteredValues();
    values[FmSynth::filterEnvelope] = 24.0f;

    for (const auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        CAPTURE (rate);
        auto synth = makeSynth (rate);
        const auto numSamples = (int) (0.05 * rate) / testBlockSize * testBlockSize; // whole blocks, about 50 ms
        const auto level = 1.0 - 0.5 * ((numSamples - 16) / (0.1 * rate));
        CHECK (cutoffAfter (*synth, values, numSamples, { noteOn (0, 45) })
               == doctest::Approx (500.0 * std::pow (2.0, 2.0 * level)).epsilon (0.003));
    }

    // Swept four octaves in 5 ms at the highest resonance, chords of soft and hard notes, at any block size: only numbers,
    // none far over what the same notes play with the filter held open.
    values[FmSynth::filterEnvelope] = 48.0f;
    values[FmSynth::filterVelocity] = 1.0f;
    values[FmSynth::filterDecay] = 5.0f;
    values[FmSynth::filterSustain] = 0.0f;
    values[FmSynth::resonance] = 1.0f;
    values[FmSynth::cutoff] = 200.0f;

    for (const auto rate : { 44100.0, 192000.0 })
    {
        CAPTURE (rate);
        auto synth = makeSynth (rate);
        std::vector<NoteEvent> notes;

        for (int i = 0; i < 12; ++i)
            notes.push_back (noteOn (i * 1777, 36 + i * 5, i % 2 == 0 ? 1.0f : 0.3f));

        const auto sound = play (*synth, values, 48000, notes);
        auto finite = true;

        for (const auto sample : sound)
            finite = finite && std::isfinite (sample);

        CHECK (finite);
        CHECK (peakOf (sound, 0, sound.size()) < 30.0f);
        CHECK (peakOf (sound, 0, sound.size()) > 0.05f);
    }
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

TEST_CASE ("A key played again on the FM synth as it rings out takes a voice of its own: its tail rings on, the new note starts afresh")
{
    // A new FM synth's electric piano at C3, where Key Time stretches its carriers' release to near half a second: let go,
    // then played again inside the release. It plays each note as it does alone, added: the first's tail as it would have
    // rung out, the second from its operators' places in their waves, as a note on a voice of its own. A voice taken back for
    // the second would have faded the tail out within a few milliseconds.
    const auto played = playApart (getDefaults(), 72000, { noteOn (0, 48), noteOff (4800, 48), noteOn (9600, 48), noteOff (19200, 48) },
                                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (9600, 48), noteOff (19200, 48) } });
    REQUIRE (peakOf (played.together, 0, 4800) > 0.05f);
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[45] == 2);  // 0.25 s: the first's tail with the second
    CHECK (played.voicesAfter[120] == 1); // 0.65 s: the tail has rung out, at 0.58 s
    CHECK (played.voicesAfter.back() == 0);
}

TEST_CASE ("A note-off lets go of its key's held voice only on the FM synth: a tail of the same key rings on, its release not begun again")
{
    // The electric piano at C3 let go, a note-off while only its tail sounds, which lets go of nothing, then C3 again, let go
    // while the first's tail still rings: that note-off lets go of the second note only. Begun again, the first's release
    // would have rung on until the second's ends, at 0.78 s.
    const auto played =
        playApart (getDefaults(), 72000, { noteOn (0, 48), noteOff (4800, 48), noteOff (7200, 48), noteOn (9600, 48), noteOff (14400, 48) },
                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (9600, 48), noteOff (14400, 48) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[45] == 2);  // the two tails
    CHECK (played.voicesAfter[120] == 1); // 0.65 s: the first's rang out at 0.58 s
    CHECK (played.voicesAfter[150] == 0);
}

TEST_CASE ("A key played again on the FM synth while it is held lets its voice go as if it came up; the new note takes a voice of its own")
{
    // Two notes of C3 that overlap: the first lets go as the second begins, its release ringing on, and the first note-off
    // lets go of the key, which then holds nothing for the second. A key never holds two voices.
    const auto played = playApart (getDefaults(), 72000, { noteOn (0, 48), noteOn (4800, 48), noteOff (9600, 48), noteOff (14400, 48) },
                                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (4800, 48), noteOff (9600, 48) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[25] == 2); // 0.14 s: the first's tail, and the second held
}

TEST_CASE ("Under the sustain pedal, a key played again on the FM synth lets the voice the pedal holds ring out, and the pedal holds the "
           "new one")
{
    // The pedal down, C3 played and let go: the pedal holds it. Played again, the voice the pedal held lets go, its release
    // ringing on, and the new note takes a voice of its own, which the pedal holds until it lifts: one voice held for the key.
    const auto played = playApart (
        getDefaults(), 72000,
        { pedal (0, true), noteOn (0, 48), noteOff (2400, 48), noteOn (4800, 48), noteOff (7200, 48), pedal (12000, false) },
        { { noteOn (0, 48), noteOff (4800, 48) }, { pedal (0, true), noteOn (4800, 48), noteOff (7200, 48), pedal (12000, false) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[30] == 2);  // 0.17 s: the first's tail, and the second that the pedal holds
    CHECK (played.voicesAfter[120] == 1); // 0.65 s: the first's tail rang out at 0.58 s; the pedal let go of the second at 0.25 s
    CHECK (played.voicesAfter[140] == 0);
}

TEST_CASE ("In mono mode a key played again on the FM synth as it rings out takes the one voice, its tail faded out, as before")
{
    // Operator 1 alone, released over 200 ms and played again a third of the way through: one voice still, whose tail fades
    // out over a few milliseconds while the note starts again from its operators' places, as a new synth's first does.
    auto values = getPlainValues();
    values[FmSynth::voiceMode] = 1.0f;
    at (values, 0, Field::release) = 200.0f;
    const auto synth = makeSynth();
    auto most = 0;
    const auto played = play (*synth, values, 36000, { noteOn (0, 57), noteOff (4800, 57), noteOn (8000, 57), noteOff (19200, 57) },
                              [&] { most = std::max (most, synth->getActiveVoices()); });
    const auto second = play (*makeSynth(), values, 36000, { noteOn (8000, 57), noteOff (19200, 57) });
    CHECK (most == 1);
    REQUIRE (peakOf (played, 7600, 8000) > 0.05f); // its tail, as it is played again
    CHECK (largestDifference (played, second, 8192) < 1.0e-6f);
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

TEST_CASE ("A note on an FM voice that Stop was fading plays at its full level, though the voice's release ended first")
{
    // Operator 1 alone, released in a millisecond: Stop just after its note began fades the voice over 10 ms, and its
    // release ends first, which left the voice fading for the next note on it, which then died in a few milliseconds.
    auto values = getPlainValues();
    at (values, 0, Field::release) = 1.0f;
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<NoteEvent> events;
    const auto playBlock = [&buffer, &events, &values] (FmSynth& synth, bool songPlays, std::vector<NoteEvent> inBlock)
    {
        buffer.clear();
        events = std::move (inBlock);
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = testBlockSize;
        block.parameters = values;
        block.events = events;
        block.isPlaying = songPlays;
        synth.process (block);
        return buffer.getMagnitude (0, 0, testBlockSize);
    };

    const auto loudestOfNote = [&playBlock] (FmSynth& synth)
    {
        playBlock (synth, true, { noteOn (0, 60) });
        auto loudest = 0.0f;

        for (int i = 0; i < 8; ++i)
            loudest = std::max (loudest, playBlock (synth, true, {}));

        return loudest;
    };

    const auto fresh = makeSynth();
    const auto full = loudestOfNote (*fresh);
    REQUIRE (full > 0.1f);

    const auto synth = makeSynth();
    playBlock (*synth, true, { noteOn (0, 60) });
    playBlock (*synth, false, { { NoteEvent::Type::allNotesOff, 0, 0, 0.0f } });

    for (int i = 0; i < 8; ++i)
        playBlock (*synth, false, {});

    REQUIRE (synth->getActiveVoices() == 0);
    CHECK (loudestOfNote (*synth) == doctest::Approx (full).epsilon (0.01));
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
