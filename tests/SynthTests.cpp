// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's synth (src/devices/synth/Synth.h): its pitch and velocity, its voices (a key played again, as it rings out,
// while it is held or under the pedal, each note on a voice of its own), filter and combs, its parts switched off, pitch
// bend, mono glide and sustain pedal, and that it plays the same every time.
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
// returns its left channel; after each block, afterBlock (the samples done) is called. The song plays in the blocks that
// begin before playingUntil.
template <typename AfterBlock>
std::vector<float> play (Synth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
                         AfterBlock afterBlock, int blockSize = testBlockSize, int playingUntil = 0)
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
        block.isPlaying = done < playingUntil;
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

// The largest step from one sample to the next.
float largestStep (const std::vector<float>& sound, size_t from, size_t to)
{
    auto largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < to && i < sound.size(); ++i)
        largest = std::max (largest, std::abs (sound[i] - sound[i - 1]));

    return largest;
}

float largestDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    auto largest = 0.0f;

    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
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
    Synth synth;
    synth.prepare (testRate, testBlockSize);
    played.together = play (synth, values, numSamples, together, [&] (int) { played.voicesAfter.push_back (synth.getActiveVoices()); });
    played.added.assign (played.together.size(), 0.0f);

    for (const auto& group : groups)
    {
        Synth alone;
        alone.prepare (testRate, testBlockSize);
        const auto part = play (alone, values, numSamples, group);

        for (size_t i = 0; i < part.size(); ++i)
            played.added[i] += part[i];
    }

    return played;
}
} // namespace

TEST_CASE ("The synth's description holds together")
{
    REQUIRE (devices::isValid (Synth::info));
    CHECK (Synth::info.kind == devices::DeviceKind::instrument);
    CHECK (Synth::info.parameters.size() == (size_t) Synth::unisonOn + 1);
    CHECK (devices::findParameter (Synth::info, "cutoff") == Synth::cutoff);
    CHECK (devices::findParameter (Synth::info, "voiceMode") == Synth::voiceMode);
    CHECK (devices::findParameter (Synth::info, "unisonVoices") == Synth::unisonVoices);
    CHECK (devices::findParameter (Synth::info, "filterSlope") == Synth::filterSlope);
    CHECK (devices::findParameter (Synth::info, "lfoOn") == Synth::lfoOn);

    // The parts that switch on and off (the owner, 2026-11-06: "the synth instrument doesn't have to have all blocks active
    // to start out with"): a new synth's filter and its envelope on, its LFO and unison off, each set to be heard as soon as
    // it is switched on.
    CHECK (Synth::info.parameters[Synth::filterOn].defaultValue == doctest::Approx (1.0f));
    CHECK (Synth::info.parameters[Synth::filterEnvelopeOn].defaultValue == doctest::Approx (1.0f));
    CHECK (Synth::info.parameters[Synth::lfoOn].defaultValue == doctest::Approx (0.0f));
    CHECK (Synth::info.parameters[Synth::unisonOn].defaultValue == doctest::Approx (0.0f));
    CHECK (Synth::info.parameters[Synth::lfoDepth].defaultValue > 0.0f);
    CHECK (Synth::info.parameters[Synth::unisonVoices].defaultValue > 0.0f);
}

TEST_CASE ("A part of the synth switched off does nothing: its filter, the filter's envelope, its LFO and its unison")
{
    const auto playSaw = [] (std::vector<float> values)
    {
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        return play (synth, values, 24000, { noteOn (0, 69) });
    };

    // A saw at 440 Hz through a low-pass at 300 Hz.
    auto values = getSineValues();
    values[Synth::osc1Wave] = 2.0f; // saw
    values[Synth::cutoff] = 300.0f;
    const auto filtered = playSaw (values);

    // The filter off: the whole saw, louder by far.
    values[Synth::filterOn] = 0.0f;
    const auto unfiltered = playSaw (values);
    CHECK (measureLevelAt (unfiltered, 4800, 24000, 440.0) > 2.0 * measureLevelAt (filtered, 4800, 24000, 440.0));
    values[Synth::filterOn] = 1.0f;

    // Its envelope off: as if it moved the cutoff by nothing, however far it is set to.
    values[Synth::filterEnvelope] = 48.0f;
    values[Synth::filterSustain] = 0.0f;
    const auto swept = playSaw (values);
    CHECK (swept != filtered);
    values[Synth::filterEnvelopeOn] = 0.0f;
    CHECK (playSaw (values) == filtered);
    values[Synth::filterEnvelopeOn] = 1.0f;
    values[Synth::filterEnvelope] = 0.0f;

    // The LFO off: its depth moves nothing, as at none.
    values[Synth::lfoTarget] = 0.0f; // pitch
    values[Synth::lfoDepth] = 1.0f;
    values[Synth::lfoOn] = 1.0f;
    CHECK (playSaw (values) != filtered);
    values[Synth::lfoOn] = 0.0f;
    CHECK (playSaw (values) == filtered);

    // The unison off: one voice, however many it is set to.
    values[Synth::unisonVoices] = 6.0f; // seven
    values[Synth::unisonOn] = 1.0f;
    CHECK (playSaw (values) != filtered);
    values[Synth::unisonOn] = 0.0f;
    CHECK (playSaw (values) == filtered);
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

TEST_CASE ("A note whose filter envelope has no attack plays its filter open from its first sample")
{
    // A saw through a 200 Hz low-pass that its envelope opens two octaves at once and holds there, against the same saw
    // through the filter set at 800 Hz, still: the two alike from the note's start, not darker for its first samples.
    auto values = getSineValues();
    values[Synth::osc1Wave] = 2.0f; // saw
    values[Synth::filterOn] = 1.0f;
    values[Synth::filterEnvelopeOn] = 1.0f;
    values[Synth::filterAttack] = 0.0f;
    values[Synth::filterSustain] = 1.0f;
    const auto playThrough = [&values] (float cutoffHertz, float envelope)
    {
        values[Synth::cutoff] = cutoffHertz;
        values[Synth::filterEnvelope] = envelope;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        return play (synth, values, 1024, { noteOn (100, 60) });
    };

    const auto opened = playThrough (200.0f, 24.0f), still = playThrough (800.0f, 0.0f);
    REQUIRE (peakOf (still, 100, 132) > 0.01f);
    auto difference = 0.0f;

    for (size_t i = 0; i < opened.size(); ++i)
        difference = std::max (difference, std::abs (opened[i] - still[i]));

    CHECK (difference < 1.0e-6f);
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

TEST_CASE ("Stopping playback cuts the notes that had only just begun, and lets the others ring out")
{
    // C3 from the start, and C5 begun 5 ms before all notes off at 0.5 s, as the loop's first notes are when Stop comes as
    // it goes round: from 20 ms to 100 ms after it, how much is left of each.
    auto values = getSineValues();
    values[Synth::release] = 200.0f;
    constexpr int offAt = 24064; // a block's start
    const std::vector<NoteEvent> events { noteOn (0, 48), noteOn (offAt - 240, 72), { NoteEvent::Type::allNotesOff, offAt, 0, 0.0f } };

    const auto playUntil = [&] (int playingUntil)
    {
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        return play (synth, values, 48000, events, [] (int) {}, testBlockSize, playingUntil);
    };

    // Stopped: C5 is gone, while C3 rings out over its release.
    const auto stopped = playUntil (offAt);
    CHECK (measureLevelAt (stopped, offAt + 960, offAt + 4800, 523.25) < 0.001);
    CHECK (measureLevelAt (stopped, offAt + 960, offAt + 4800, 130.81) > 0.1);

    // A jump while playing lets both ring out.
    const auto jumped = playUntil (48000);
    CHECK (measureLevelAt (jumped, offAt + 960, offAt + 4800, 523.25) > 0.1);
    CHECK (measureLevelAt (jumped, offAt + 960, offAt + 4800, 130.81) > 0.1);
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

TEST_CASE ("A key played again as it rings out takes a voice of its own: its tail rings on, and the new note starts afresh")
{
    // A new synth's two detuned saws through its filter, which its envelope opens and closes: C3 let go, then played again a
    // third of the way through its 200 ms release. It plays each note as it does alone, added: the first's tail as it would
    // have rung out, the second from the start of its waves and of its envelopes, as LMMS and most polyphonic synths play a
    // key again. Its voice taken back, the second would have gone on with the first's waves and envelopes.
    const auto played = playApart (getDefaults(), 36000, { noteOn (0, 48), noteOff (4800, 48), noteOn (8000, 48), noteOff (19200, 48) },
                                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (8000, 48), noteOff (19200, 48) } });
    REQUIRE (peakOf (played.together, 0, 4800) > 0.1f);
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[40] == 2); // 0.22 s: the first's tail with the second
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the tail has rung out, at 0.3 s
    CHECK (played.voicesAfter.back() == 0);
}

TEST_CASE ("A note-off lets go of its key's held voice only: a tail of the same key rings on, its release not begun again")
{
    // C3 let go, a note-off while only its tail sounds, which lets go of nothing, then C3 again, let go while the first's tail
    // still rings: that note-off lets go of the second note only. Begun again, the first's release would have rung on past
    // the 200 ms it takes.
    const auto played =
        playApart (getDefaults(), 36000, { noteOn (0, 48), noteOff (4800, 48), noteOff (6400, 48), noteOn (8000, 48), noteOff (9600, 48) },
                   { { noteOn (0, 48), noteOff (4800, 48) }, { noteOn (8000, 48), noteOff (9600, 48) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[40] == 2); // the two tails
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the first's rang out at 0.3 s, the second's rings until 0.4 s
    CHECK (played.voicesAfter[76] == 0);
}

TEST_CASE ("A key played again while it is held lets its voice go as if it came up, and the new note takes a voice of its own")
{
    // Two notes of C3 that overlap, as two keyboards, or the song and a player on its track, can send them: the first lets go
    // as the second begins, its release ringing on, and the first note-off lets go of the key, which then holds nothing for
    // the second. A key never holds two voices, so it never plays twice as loud, as the song's own notes never do (a note
    // there ends where the same pitch starts again).
    const auto played = playApart (getDefaults(), 36000, { noteOn (0, 48), noteOn (3200, 48), noteOff (4800, 48), noteOff (8000, 48) },
                                   { { noteOn (0, 48), noteOff (3200, 48) }, { noteOn (3200, 48), noteOff (4800, 48) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[14] == 2); // 0.08 s: the first's tail, and the second held
}

TEST_CASE ("Under the sustain pedal, a key played again lets the voice the pedal holds ring out, and the pedal holds the new one")
{
    // The pedal down, C3 played and let go: the pedal holds it. Played again, the voice the pedal held lets go, its release
    // ringing on, and the new note takes a voice of its own, which the pedal holds in turn until it lifts, as JUCE's
    // synthesiser and Serum 2 play a key struck again under the pedal. So a key played again and again under the pedal holds
    // one voice, as a piano's key holds one string, not one for each time it was struck, which would soon take the voices
    // the pedal holds for the other keys.
    const auto played = playApart (
        getDefaults(), 36000,
        { pedal (0, true), noteOn (0, 48), noteOff (1600, 48), noteOn (4800, 48), noteOff (6400, 48), pedal (9600, false) },
        { { noteOn (0, 48), noteOff (4800, 48) }, { pedal (0, true), noteOn (4800, 48), noteOff (6400, 48), pedal (9600, false) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[30] == 2); // 0.17 s: the first's tail, and the second that the pedal holds
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the first's tail rang out at 0.3 s; the pedal let go of the second at 0.2 s
    CHECK (played.voicesAfter[76] == 0);
}

TEST_CASE ("In mono mode a key played again as it rings out takes the one voice back from where it is, as before")
{
    // A sine, released over 200 ms and played again a third of the way through: one voice still, its wave going on where it
    // was and its level rising from where its release had brought it, without a step (a sine at 131 Hz moves at most 0.02 a
    // sample, its attack's rise with it; started again, the wave would have jumped by its level, 0.2 here).
    auto values = getSineValues();
    values[Synth::voiceMode] = 1.0f;
    Synth synth;
    synth.prepare (testRate, testBlockSize);
    auto most = 0;
    const auto played = play (synth, values, 36000, { noteOn (0, 48), noteOff (4800, 48), noteOn (8000, 48), noteOff (19200, 48) },
                              [&] (int) { most = std::max (most, synth.getActiveVoices()); });
    CHECK (most == 1);
    REQUIRE (peakOf (played, 7600, 8000) > 0.1f); // its tail, as it is played again
    CHECK (largestStep (played, 7900, 8300) < 0.05f);
}

TEST_CASE ("A slide note glides the sounding notes to its pitch evenly over its length, and a portamento note glides in")
{
    // C4, with a slide to C5 a quarter of a second in, half a second long: half way there half way through, there after.
    const auto values = getSineValues();
    Synth slid;
    slid.prepare (testRate, testBlockSize);
    auto played = play (slid, values, 72000, { noteOn (0, 60), { NoteEvent::Type::slide, 12000, 72, 0.5f } });
    CHECK (measureFrequency (played, 2400, 11000) == doctest::Approx (261.63).epsilon (0.002));
    CHECK (measureFrequency (played, 23000, 25000) == doctest::Approx (261.63 * std::sqrt (2.0)).epsilon (0.03));
    CHECK (measureFrequency (played, 40000, 72000) == doctest::Approx (523.25).epsilon (0.002));
    CHECK (slid.getActiveVoices() == 1); // the slide played no note of its own

    // A portamento note after C4 starts there and glides up to its own pitch in a tenth of a second (Glide is off). In mono
    // mode, so that C4's release doesn't sound with it.
    auto mono = values;
    mono[Synth::voiceMode] = 1.0f;
    Synth porta;
    porta.prepare (testRate, testBlockSize);
    played = play (porta, mono, 72000, { noteOn (0, 60), noteOff (24000, 60), { NoteEvent::Type::portamentoOn, 24000, 72, 1.0f } });
    CHECK (measureFrequency (played, 24000, 25200) < 380.0);
    CHECK (measureFrequency (played, 36000, 72000) == doctest::Approx (523.25).epsilon (0.002));

    // A plain note after C4 starts on its own pitch.
    Synth plain;
    plain.prepare (testRate, testBlockSize);
    played = play (plain, mono, 48000, { noteOn (0, 60), noteOff (24000, 60), noteOn (24000, 72) });
    CHECK (measureFrequency (played, 24480, 30000) == doctest::Approx (523.25).epsilon (0.002));
}

TEST_CASE ("The synth's pitch envelope drops each note onto its pitch over its decay, as an 808 drops")
{
    // An octave up at the start, falling with a 20 ms time constant: near A5 in the first few milliseconds, A4 by 200 ms.
    auto values = getSineValues();
    values[Synth::pitchAmount] = 12.0f;
    values[Synth::pitchDecay] = 20.0f;
    Synth synth;
    synth.prepare (testRate, testBlockSize);
    const auto played = play (synth, values, 24000, { noteOn (0, 69) });
    CHECK (measureFrequency (played, 0, 240) > 800.0);
    CHECK (measureFrequency (played, 9600, 24000) == doctest::Approx (440.0).epsilon (0.002));

    // And a legato note in mono mode doesn't start it again.
    values[Synth::voiceMode] = 1.0f;
    Synth mono;
    mono.prepare (testRate, testBlockSize);
    const auto legato = play (mono, values, 24000, { noteOn (0, 69), noteOn (9600, 57) });
    CHECK (measureFrequency (legato, 9840, 24000) == doctest::Approx (220.0).epsilon (0.002));
}

TEST_CASE ("Osc 2 synced to osc 1 repeats at osc 1's pitch, and the filter envelope sweeps it")
{
    // Osc 2 alone, a fifth above osc 1: unsynced it plays its own pitch; synced, its cycle starts again with each of osc 1's,
    // so what comes out repeats at osc 1's pitch (A3), with osc 2's harmonics around its own.
    auto values = getSineValues();
    values[Synth::osc2Wave] = 2.0f; // saw
    values[Synth::osc2Pitch] = 7.0f;
    values[Synth::osc2Detune] = 0.0f;
    values[Synth::oscMix] = 1.0f;
    const auto atPitch = [&values] (bool synced, double frequency)
    {
        auto set = values;
        set[Synth::osc2Sync] = synced ? 1.0f : 0.0f;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, set, 24000, { noteOn (0, 57) });
        return measureLevelAt (played, 4800, 24000, frequency);
    };

    CHECK (atPitch (false, 220.0) < 0.01);
    CHECK (atPitch (true, 220.0) > 0.1);
    CHECK (atPitch (false, 220.0 * std::exp2 (7.0 / 12.0)) > 0.3);

    // Osc 2's envelope: the filter envelope, at its peak, takes osc 2 an octave up, then lets it back down to where it is set.
    values[Synth::osc2Pitch] = 0.0f;
    values[Synth::osc2Envelope] = 12.0f;
    values[Synth::filterAttack] = 0.0f;
    values[Synth::filterDecay] = 100.0f;
    values[Synth::filterSustain] = 0.0f;
    Synth swept;
    swept.prepare (testRate, testBlockSize);
    const auto played = play (swept, values, 48000, { noteOn (0, 57) });
    CHECK (measureFrequency (played, 0, 480) > 380.0);
    CHECK (measureFrequency (played, 24000, 48000) == doctest::Approx (220.0).epsilon (0.002));

    // Switched off, the filter envelope doesn't sweep it.
    values[Synth::filterEnvelopeOn] = 0.0f;
    Synth still;
    still.prepare (testRate, testBlockSize);
    CHECK (measureFrequency (play (still, values, 4800, { noteOn (0, 57) }), 0, 4800) == doctest::Approx (220.0).epsilon (0.002));
}

TEST_CASE ("The synth's sub plays an octave or two down, as a sine, triangle, saw or square")
{
    auto values = getSineValues();
    values[Synth::oscMix] = 0.0f;
    values[Synth::subLevel] = 1.0f;
    const auto subAt = [&values] (int octave, int shape, double frequency)
    {
        auto set = values;
        set[Synth::subOctave] = (float) octave;
        set[Synth::subShape] = (float) shape;
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        return measureLevelAt (play (synth, set, 48000, { noteOn (0, 69) }), 4800, 48000, frequency);
    };

    CHECK (subAt (0, 0, 220.0) > 0.9);  // a sine an octave down, as always
    CHECK (subAt (1, 0, 110.0) > 0.9);  // two octaves down
    CHECK (subAt (1, 0, 220.0) < 0.01); // and not one
    CHECK (subAt (0, 3, 660.0) > 0.3);  // a square: its third harmonic, a third of its first
    CHECK (subAt (0, 0, 660.0) < 0.01); // which a sine hasn't
}

TEST_CASE ("In Legato glide mode only a note played while another is held glides, as a TB-303 slides")
{
    auto values = getSineValues();
    values[Synth::voiceMode] = 1.0f;
    values[Synth::glide] = 100.0f;
    values[Synth::glideMode] = 1.0f; // legato

    // C4, let go, then C5: no glide, C5 at once.
    Synth apart;
    apart.prepare (testRate, testBlockSize);
    auto played = play (apart, values, 36000, { noteOn (0, 60), noteOff (12000, 60), noteOn (12000, 72) });
    CHECK (measureFrequency (played, 12240, 14400) == doctest::Approx (523.25).epsilon (0.003));

    // C4 held, then C5: it glides there.
    Synth held;
    held.prepare (testRate, testBlockSize);
    played = play (held, values, 60000, { noteOn (0, 60), noteOn (12000, 72) });
    CHECK (measureFrequency (played, 12240, 14400) < 480.0);
    CHECK (measureFrequency (played, 50000, 60000) == doctest::Approx (523.25).epsilon (0.002));

    // Always (as before), the note let go of still glides into the next.
    values[Synth::glideMode] = 0.0f;
    Synth always;
    always.prepare (testRate, testBlockSize);
    played = play (always, values, 36000, { noteOn (0, 60), noteOff (12000, 60), noteOn (12000, 72) });
    CHECK (measureFrequency (played, 12240, 14400) < 480.0);
}

TEST_CASE ("The filter's velocity closes the cutoff for softer notes")
{
    // A saw through a low-pass at 2 kHz: at full velocity as set; at velocity 0.5 with full velocity tracking, two octaves
    // lower, so its harmonics around 2 kHz are much weaker.
    auto values = getDefaults();
    values[Synth::osc1Wave] = 2.0f;
    values[Synth::oscMix] = 0.0f;
    values[Synth::cutoff] = 2000.0f;
    values[Synth::filterEnvelope] = 0.0f;
    values[Synth::keyTracking] = 0.0f;
    values[Synth::velocity] = 0.0f;
    values[Synth::filterVelocity] = 1.0f;
    const auto bright = [&values] (float velocity)
    {
        Synth synth;
        synth.prepare (testRate, testBlockSize);
        const auto played = play (synth, values, 24000, { noteOn (0, 45, velocity) });
        return measureLevelAt (played, 4800, 24000, 1980.0) / measureLevelAt (played, 4800, 24000, 110.0);
    };

    CHECK (bright (0.5f) < 0.2 * bright (1.0f));

    // Off, velocity doesn't change the cutoff.
    values[Synth::filterVelocity] = 0.0f;
    CHECK (bright (0.5f) == doctest::Approx (bright (1.0f)).epsilon (0.01));
}

TEST_CASE ("The synth's LFO moves the pulse width, and comes in over its fade after each note starts")
{
    // A square whose width the LFO sweeps: its second harmonic, which a square at 50 % hasn't, comes in as it thins.
    auto values = getSineValues();
    values[Synth::osc1Wave] = 3.0f; // square
    values[Synth::lfoOn] = 1.0f;
    values[Synth::lfoTarget] = 3.0f; // pulse width
    values[Synth::lfoShape] = 0.0f;
    values[Synth::lfoRate] = 2.0f;
    values[Synth::lfoDepth] = 1.0f;
    Synth swept;
    swept.prepare (testRate, testBlockSize);
    const auto played = play (swept, values, 48000, { noteOn (0, 57) });
    CHECK (measureLevelAt (played, 4000, 8000, 440.0) > 0.1); // a quarter of the LFO's cycle in: thin

    values[Synth::lfoOn] = 0.0f;
    Synth plain;
    plain.prepare (testRate, testBlockSize);
    CHECK (measureLevelAt (play (plain, values, 48000, { noteOn (0, 57) }), 4000, 8000, 440.0) < 0.01);

    // Vibrato over a fade of half a second: none at first, all of it once the fade is done.
    auto vibrato = getSineValues();
    vibrato[Synth::lfoOn] = 1.0f;
    vibrato[Synth::lfoTarget] = 0.0f; // pitch
    vibrato[Synth::lfoShape] = 4.0f;  // square: a semitone up, then down
    vibrato[Synth::lfoRate] = 1.0f;
    vibrato[Synth::lfoDepth] = 1.0f;
    vibrato[Synth::lfoFade] = 500.0f;
    Synth faded;
    faded.prepare (testRate, testBlockSize);
    const auto heard = play (faded, vibrato, 48000, { noteOn (0, 69) });
    CHECK (measureFrequency (heard, 0, 1200) == doctest::Approx (440.0).epsilon (0.01));
    CHECK (measureFrequency (heard, 26400, 33600) == doctest::Approx (440.0 * std::exp2 (-1.0 / 12.0)).epsilon (0.003));
}

TEST_CASE ("Drift lets each voice's pitch wander a few cents, the same every time")
{
    auto values = getSineValues();
    values[Synth::drift] = 1.0f;
    Synth first, second;
    first.prepare (testRate, testBlockSize);
    second.prepare (testRate, testBlockSize);
    const auto played = play (first, values, 48000, { noteOn (0, 69) });
    CHECK (play (second, values, 48000, { noteOn (0, 69) }) == played);

    // Off by a few cents, at most the drift's eight, and never quite where it was.
    const auto cents = [] (double frequency) { return 1200.0 * std::log2 (frequency / 440.0); };
    const auto early = cents (measureFrequency (played, 0, 9600)), late = cents (measureFrequency (played, 38400, 48000));
    CHECK (std::abs (early) <= 8.5);
    CHECK (std::abs (late) <= 8.5);
    CHECK (std::abs (early - late) > 0.1);

    // Without it, A4 exactly.
    values[Synth::drift] = 0.0f;
    Synth still;
    still.prepare (testRate, testBlockSize);
    CHECK (std::abs (cents (measureFrequency (play (still, values, 48000, { noteOn (0, 69) }), 0, 48000))) < 0.05);
}

TEST_CASE ("The synth's filter drive saturates what goes into the filter, adding harmonics to a sine, and only while the filter is on")
{
    // A sine through the open filter: driven 18 dB, it gains its third harmonic; at 0 dB, it plays as before, bit for bit.
    auto values = getSineValues();
    const auto thirdOf = [] (const std::vector<float>& played)
    { return measureLevelAt (played, 4800, 24000, 660.0) / measureLevelAt (played, 4800, 24000, 220.0); };

    Synth clean, driven;
    clean.prepare (testRate, testBlockSize);
    driven.prepare (testRate, testBlockSize);
    const auto plain = play (clean, values, 24000, { noteOn (0, 57) });
    CHECK (thirdOf (plain) < 0.001);

    values[Synth::drive] = 18.0f;
    CHECK (thirdOf (play (driven, values, 24000, { noteOn (0, 57) })) > 0.05);

    values[Synth::drive] = 0.0f;
    Synth again;
    again.prepare (testRate, testBlockSize);
    CHECK (play (again, values, 24000, { noteOn (0, 57) }) == plain);

    // With the filter switched off, the drive is off with it.
    values[Synth::drive] = 18.0f;
    values[Synth::filterOn] = 0.0f;
    Synth off;
    off.prepare (testRate, testBlockSize);
    CHECK (thirdOf (play (off, values, 24000, { noteOn (0, 57) })) < 0.001);
}
