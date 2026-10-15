// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's synth (src/devices/synth/Synth.h): its pitch and velocity, its voices, filter, pitch bend, mono glide and
// sustain pedal, and that it plays the same every time.
#include "TestHelpers.h"
#include "devices/dsp/Math.h"
#include "devices/synth/Synth.h"

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

// Plays the synth in blocks, with events at their samples from the start, and returns its left channel; after each
// block, afterBlock (the samples done) is called.
template <typename AfterBlock>
std::vector<float> play (Synth& synth, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
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
