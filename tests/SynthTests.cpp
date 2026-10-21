// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's synth (src/devices/synth/Synth.h): its pitch and velocity, its voices, filter and combs, pitch bend, mono
// glide and sustain pedal, and that it plays the same every time.
#include "TestHelpers.h"
#include "devices/dsp/Math.h"
#include "devices/synth/Synth.h"

#include <array>
#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::NoteEvent;
using devices::Synth;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// A plain sine at full level: no second oscillator, the filter open and still, no velocity, full sustain.
std::vector<float> getSineValues()
{
    auto values = getDefaults();
    values[Synth::osc1Wave] = 0.0f; // sine
    values[Synth::oscMix] = 0.0f;
    values[Synth::cutoff] = 20000.0f;
    values[Synth::resonance] = 0.0f;
    values[Synth::filterEnvelope] = 0.0f;
    values[Synth::keyTracking] = 0.0f;
    values[Synth::sustain] = 1.0f;
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

// Plays the synth in blocks (of testBlockSize samples unless given), with events at their samples from the start, and
// returns its left channel; after each block, afterBlock (the samples done) is called.
template <typename AfterBlock>
std::vector<float> play (Synth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
                         AfterBlock afterBlock, int blockSize = testBlockSize)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> played;
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
        synth.process (block);
        afterBlock (done + length);

        for (int i = 0; i < length; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}

std::vector<float> play (Synth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events)
{
    return play (synth, values, numSamples, events, [] (int) {});
}

// The frequency of a steady tone between two times, from its rising zero crossings.
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

// How strong a frequency is in a stretch of sound, as an amplitude: its DFT at that frequency (Goertzel), through a Hann
// window so that the notes beside it don't leak in.
double measureLevelAt (const std::vector<float>& sound, size_t from, size_t to, double frequency)
{
    const auto coefficient = 2.0 * std::cos (juce::MathConstants<double>::twoPi * frequency / testRate);
    const auto length = (double) (to - from);
    auto s1 = 0.0, s2 = 0.0, weights = 0.0;

    for (auto i = from; i < to; ++i)
    {
        const auto weight = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) (i - from) / length);
        const auto s0 = weight * sound[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
        weights += weight;
    }

    const auto power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt (std::max (power, 0.0)) / weights;
}

float peakOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < to && i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}
} // namespace

TEST_CASE ("The synth's description holds together")
{
    REQUIRE (devices::isValid (Synth::info));
    CHECK (Synth::info.kind == devices::DeviceKind::instrument);
    CHECK (Synth::info.parameters.size() == (size_t) Synth::level + 1);
    CHECK (devices::findParameter (Synth::info, "cutoff") == Synth::cutoff);
    CHECK (devices::findParameter (Synth::info, "voiceMode") == Synth::voiceMode);
    CHECK (devices::findParameter (Synth::info, "unisonVoices") == Synth::unisonVoices);
    CHECK (devices::findParameter (Synth::info, "filterSlope") == Synth::filterSlope);
}

TEST_CASE ("A synth note plays at its pitch, as loud as its velocity says")
{
    auto values = getSineValues();
    Synth synth;
    synth.prepare (testRate, testBlockSize);
    auto played = play (synth, values, 48000, { noteOn (0, 69) });
    CHECK (measureFrequency (played, 4800, 48000) == doctest::Approx (440.0).epsilon (0.001));
    CHECK (peakOf (played, 4800, 48000) == doctest::Approx (1.0f).epsilon (0.01));

    // With velocity fully counted, half velocity is a quarter of the level: velocity counts squared, as keyboards feel.
    values[Synth::velocity] = 1.0f;
    Synth soft;
    soft.prepare (testRate, testBlockSize);
    played = play (soft, values, 9600, { noteOn (0, 69, 0.5f) });
    CHECK (peakOf (played, 4800, 9600) == doctest::Approx (0.25f).epsilon (0.01));
}

TEST_CASE ("The synth plays chords, rings notes out over their release, and steals the oldest voice when it runs out")
{
    auto values = getSineValues();
    values[Synth::release] = 100.0f;

    Synth synth;
    synth.prepare (testRate, testBlockSize);
    std::vector<int> voicesAfter;
    const auto played =
        play (synth, values, 24000,
              { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOff (9600, 60), noteOff (9600, 64), noteOff (9600, 67) },
              [&] (int) { voicesAfter.push_back (synth.getActiveVoices()); });

    // Each note of a C major chord, and nothing between them.
    for (const auto note : { 60, 64, 67 })
        CHECK (measureLevelAt (played, 2400, 9600, devices::dsp::noteToFrequency (note)) == doctest::Approx (1.0).epsilon (0.05));

    CHECK (measureLevelAt (played, 2400, 9600, 300.0) < 0.05);
    CHECK (voicesAfter[10] == 3);
    CHECK (voicesAfter.back() == 0); // the 100 ms release is over

    // Seventeen keys at once: sixteen voices, the first note taken for the last.
    Synth crowded;
    crowded.prepare (testRate, testBlockSize);
    std::vector<NoteEvent> keys;

    for (int note = 60; note < 77; ++note)
        keys.push_back (noteOn (note - 60, note));

    // Almost a second, to tell semitones apart.
    const auto all = play (crowded, values, 48000, keys);
    CHECK (crowded.getActiveVoices() == Synth::maxVoices);
    CHECK (measureLevelAt (all, 4800, 48000, devices::dsp::noteToFrequency (60)) < 0.05);
    CHECK (measureLevelAt (all, 4800, 48000, devices::dsp::noteToFrequency (61)) > 0.5);
    CHECK (measureLevelAt (all, 4800, 48000, devices::dsp::noteToFrequency (76)) > 0.5);
}

TEST_CASE ("The synth's filter darkens its saw, and the filter envelope opens it")
{
    auto values = getSineValues();
    values[Synth::osc1Wave] = 2.0f; // saw
    const auto trebleOf = [&values] (float cutoffHertz, float envelope)
    {
        values[Synth::cutoff] = cutoffHertz;
        values[Synth::filterEnvelope] = envelope;
        values[Synth::filterAttack] = 0.0f;
        values[Synth::filterDecay] = 300.0f;
        values[Synth::filterSustain] = 0.0f;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, values, 48000, { noteOn (0, 45) }); // A2, 110 Hz
        // The 27th harmonic, early in the note and late.
        return std::pair { measureLevelAt (played, 0, 2400, 2970.0), measureLevelAt (played, 38400, 48000, 2970.0) };
    };

    const auto open = trebleOf (20000.0f, 0.0f), closed = trebleOf (500.0f, 0.0f);
    CHECK (closed.second < 0.1 * open.second);

    // An envelope of +48 semitones opens the 500 Hz filter at the note's start and closes it as it decays.
    const auto swept = trebleOf (500.0f, 48.0f);
    CHECK (swept.first > 5.0 * swept.second);
}

TEST_CASE ("The synth's combs: on a note's pitch, Comb + keeps a saw's harmonics and Comb - leaves out its even ones")
{
    // The types every Phazite filter offers (dsp/MultimodeFilter.h), in its order.
    const auto& types = Synth::info.parameters[Synth::filterType].choices;
    REQUIRE (types.size() == 9);
    CHECK (types[(size_t) devices::dsp::FilterMode::highPass] == std::string_view ("High Pass"));
    CHECK (types[(size_t) devices::dsp::FilterMode::combPlus] == std::string_view ("Comb +"));
    CHECK (types[(size_t) devices::dsp::FilterMode::combMinus] == std::string_view ("Comb -"));

    // A saw at A3 (220 Hz) through a comb tuned to it: its first four harmonics, as amplitudes.
    auto values = getSineValues();
    values[Synth::osc1Wave] = 2.0f; // saw
    values[Synth::cutoff] = 220.0f;
    values[Synth::resonance] = 0.5f;
    const auto harmonicsThrough = [&values] (devices::dsp::FilterMode type)
    {
        values[Synth::filterType] = (float) type;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, values, 24000, { noteOn (0, 57) });
        std::array<double, 4> levels {};

        for (size_t h = 0; h < levels.size(); ++h)
            levels[h] = measureLevelAt (played, 4800, 24000, 220.0 * (double) (h + 1));

        return levels;
    };

    // Comb +: all of them on its peaks, as loud against each other as in a saw.
    const auto plus = harmonicsThrough (devices::dsp::FilterMode::combPlus);
    CHECK (plus[1] / plus[0] == doctest::Approx (1.0 / 2.0).epsilon (0.1));
    CHECK (plus[2] / plus[0] == doctest::Approx (1.0 / 3.0).epsilon (0.1));
    CHECK (plus[3] / plus[0] == doctest::Approx (1.0 / 4.0).epsilon (0.1));

    // Comb -: the odd ones on its peaks, the even ones gone: hollow, as a square wave is.
    const auto minus = harmonicsThrough (devices::dsp::FilterMode::combMinus);
    CHECK (minus[2] / minus[0] == doctest::Approx (1.0 / 3.0).epsilon (0.1));
    CHECK (minus[1] < 0.01 * minus[0]);
    CHECK (minus[3] < 0.01 * minus[0]);
}

TEST_CASE ("The synth's notch and 24 dB slope, and its vowel, phaser and sample and hold")
{
    const auto& types = Synth::info.parameters[Synth::filterType].choices;
    CHECK (types[(size_t) devices::dsp::FilterMode::notch] == std::string_view ("Notch"));
    CHECK (types[(size_t) devices::dsp::FilterMode::vowel] == std::string_view ("Vowel"));
    CHECK (types[(size_t) devices::dsp::FilterMode::phaser] == std::string_view ("Phaser"));
    CHECK (types[(size_t) devices::dsp::FilterMode::sampleAndHold] == std::string_view ("Sample & Hold"));

    // The slope starts at 12 dB, the synth's gentler sound.
    CHECK (Synth::info.parameters[Synth::filterSlope].id == std::string_view ("filterSlope"));
    CHECK (Synth::info.parameters[Synth::filterSlope].defaultValue == doctest::Approx (0.0f));

    // A saw at A2 (110 Hz) through a low-pass at 500 Hz: at 24 dB, its 27th harmonic falls much further.
    auto values = getSineValues();
    values[Synth::osc1Wave] = 2.0f; // saw
    values[Synth::cutoff] = 500.0f;
    const auto trebleAt = [&values] (float slope)
    {
        values[Synth::filterSlope] = slope;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, values, 24000, { noteOn (0, 45) });
        return measureLevelAt (played, 4800, 24000, 2970.0);
    };

    CHECK (trebleAt (1.0f) < 0.1 * trebleAt (0.0f));

    // The notch takes out a sine at its frequency (A4, 440 Hz).
    values = getSineValues();
    const auto rmsThrough = [&values] (devices::dsp::FilterMode type, float frequency)
    {
        values[Synth::filterType] = (float) type;
        values[Synth::cutoff] = frequency;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, values, 24000, { noteOn (0, 69) });
        auto sum = 0.0;

        for (size_t i = 12000; i < played.size(); ++i)
            sum += (double) played[i] * played[i];

        return std::sqrt (sum / (double) (played.size() - 12000));
    };

    CHECK (rmsThrough (devices::dsp::FilterMode::notch, 440.0f) < 0.05 * rmsThrough (devices::dsp::FilterMode::lowPass, 20000.0f));

    // The vowel, the phaser and the sample and hold play a saw, steadily.
    values = getSineValues();
    values[Synth::osc1Wave] = 2.0f;
    values[Synth::resonance] = 1.0f;

    for (const auto type : { devices::dsp::FilterMode::vowel, devices::dsp::FilterMode::phaser, devices::dsp::FilterMode::sampleAndHold })
    {
        INFO ("type " << (int) type);
        values[Synth::filterType] = (float) type;
        values[Synth::cutoff] = 1000.0f;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, values, 24000, { noteOn (0, 45), noteOn (2400, 52) });
        const auto peak = peakOf (played, 0, played.size());
        CHECK (std::isfinite (peak));
        CHECK (peak > 0.05f);
        CHECK (peak < 4.0f);
    }
}

TEST_CASE ("The synth bends its pitch, and glides from key to key in mono mode without starting again")
{
    auto values = getSineValues();

    // A full bend up of a 2-semitone range.
    Synth bent;
    bent.prepare (testRate, testBlockSize);
    auto played = play (bent, values, 24000, { { NoteEvent::Type::pitchBend, 0, 0, 1.0f }, noteOn (0, 69) });
    CHECK (measureFrequency (played, 4800, 24000) == doctest::Approx (440.0 * std::pow (2.0, 2.0 / 12.0)).epsilon (0.001));

    // Mono, a 50 ms glide: C4 held, then C5 while it is still held, so the voice glides and doesn't start again.
    values[Synth::voiceMode] = 1.0f;
    values[Synth::glide] = 50.0f;
    Synth mono;
    mono.prepare (testRate, testBlockSize);
    played = play (mono, values, 48000, { noteOn (0, 60), noteOn (24000, 72) });
    CHECK (mono.getActiveVoices() == 1);
    CHECK (measureFrequency (played, 4800, 24000) == doctest::Approx (261.63).epsilon (0.002));
    CHECK (measureFrequency (played, 36000, 48000) == doctest::Approx (523.25).epsilon (0.002));
    CHECK (peakOf (played, 24000, 24480) > 0.95f); // no new attack: the level stays up

    // Letting go of C5 goes back to C4, which is still held.
    Synth back;
    back.prepare (testRate, testBlockSize);
    played = play (back, values, 48000, { noteOn (0, 60), noteOn (4800, 72), noteOff (9600, 72) });
    CHECK (measureFrequency (played, 24000, 48000) == doctest::Approx (261.63).epsilon (0.002));
}

TEST_CASE ("The synth's glide keeps its time, however blocks and notes split the samples")
{
    // Mono, a 100 ms glide from C4 up to C5: played in the usual blocks, and in blocks of 37 samples.
    auto values = getSineValues();
    values[Synth::voiceMode] = 1.0f;
    values[Synth::glide] = 100.0f;
    const std::vector<NoteEvent> events { noteOn (0, 60), noteOn (4800, 72) };

    Synth inBlocks, inPieces;
    inBlocks.prepare (testRate, testBlockSize);
    inPieces.prepare (testRate, testBlockSize);
    const auto steady = play (inBlocks, values, 14400, events, [] (int) {});
    const auto split = play (inPieces, values, 14400, events, [] (int) {}, 37);

    // Halfway through the glide, both are at the same pitch.
    const auto expected = measureFrequency (steady, 7000, 8000);
    CHECK (expected > 300.0);
    CHECK (expected < 500.0);
    CHECK (measureFrequency (split, 7000, 8000) == doctest::Approx (expected).epsilon (0.002));
}

TEST_CASE ("A note that takes a sounding voice fades it out, and starts from silence")
{
    // All 16 voices hold a note; a 17th takes the oldest. With a 50 ms attack, the new note rises from silence.
    auto values = getSineValues();
    values[Synth::attack] = 50.0f;
    std::vector<NoteEvent> events;

    for (int n = 0; n < Synth::maxVoices; ++n)
        events.push_back (noteOn (0, 40 + n));

    events.push_back (noteOn (4800, 80));
    Synth synth;
    synth.prepare (testRate, testBlockSize);
    std::vector<int> voicesAfter;
    const auto played = play (synth, values, 14400, events, [&] (int) { voicesAfter.push_back (synth.getActiveVoices()); });

    // Just after the 17th note, the voice it took fades out besides the 16; 3 ms later it is gone.
    CHECK (voicesAfter[18] == Synth::maxVoices + 1); // the block from 4608 to 4864
    CHECK (voicesAfter[19] == Synth::maxVoices);

    // The new note (830.6 Hz) starts quietly, then reaches its level.
    const auto rising = measureLevelAt (played, 4810, 5050, 830.6);
    const auto risen = measureLevelAt (played, 9600, 14400, 830.6);
    CHECK (risen > 0.5);
    CHECK (rising < 0.3 * risen);
}

TEST_CASE ("The synth's sustain pedal holds notes until it lifts, and it plays the same every time")
{
    auto values = getSineValues();
    values[Synth::release] = 50.0f;
    const NoteEvent pedalDown { NoteEvent::Type::controller, 0, 64, 1.0f };
    const NoteEvent pedalUp { NoteEvent::Type::controller, 24000, 64, 0.0f };

    Synth synth;
    synth.prepare (testRate, testBlockSize);
    std::vector<int> voicesAfter;
    play (synth, values, 36000, { pedalDown, noteOn (0, 60), noteOff (4800, 60), pedalUp },
          [&] (int) { voicesAfter.push_back (synth.getActiveVoices()); });
    CHECK (voicesAfter[80] == 1); // 0.43 s: the key is up, the pedal down
    CHECK (voicesAfter.back() == 0);

    // Noise, two detuned saws and a random LFO on the cutoff: the same sound twice, bit for bit.
    values = getDefaults();
    values[Synth::noiseLevel] = 0.3f;
    values[Synth::lfoShape] = 5.0f;  // random
    values[Synth::lfoTarget] = 1.0f; // cutoff
    values[Synth::lfoDepth] = 0.5f;
    const std::vector<NoteEvent> riff { noteOn (0, 48), noteOn (4800, 55), noteOff (9600, 48), noteOn (12000, 60), noteOff (24000, 55) };
    Synth first, second;
    first.prepare (testRate, testBlockSize);
    second.prepare (testRate, testBlockSize);
    CHECK (play (first, values, 36000, riff) == play (second, values, 36000, riff));

    // All notes off lets everything go.
    play (first, values, 24000, { { NoteEvent::Type::allNotesOff, 0, 0, 0.0f } }); // and the 200 ms release passes
    CHECK (first.getActiveVoices() == 0);
}
