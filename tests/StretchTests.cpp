// SPDX-License-Identifier: AGPL-3.0-or-later
// How Phazite stretches audio (src/devices/stretch/Stretch.h; roadmap step 2.18): drum hits come out once each and on
// their beats, a held chord stays clean between its notes, pitch moves by semitones, and backwards is backwards. Each
// drum hit is noise of its own, so that every copy of it can be found in what comes out by correlating with its start:
// SoundTouch, which Tracktion stretched clips with, played up to half of them twice (docs/design.md, section 23).
#include "TestHelpers.h"
#include "devices/stretch/Stretch.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>

namespace
{
constexpr double rate = 44100.0;

// A sound, and where each of its hits starts.
struct Hits
{
    juce::AudioBuffer<float> audio;
    std::vector<int> starts;
};

// A hit every so often, each noise of its own, from a sharp attack falling away.
Hits makeHits (double seconds, double every, double decaySeconds)
{
    Hits hits;
    const auto length = (int) std::lround (seconds * rate);
    hits.audio.setSize (1, length);
    hits.audio.clear();
    juce::Random random (7);

    for (int hit = 0;; ++hit)
    {
        const auto start = (int) std::lround (hit * every * rate);

        if (start >= length)
            break;

        hits.starts.push_back (start);

        for (int i = start; i < std::min (length, start + (int) (8.0 * decaySeconds * rate)); ++i)
            hits.audio.addSample (0, i,
                                  (float) (0.4 * std::exp (-(i - start) / (decaySeconds * rate))) * (random.nextFloat() * 2.0f - 1.0f));
    }

    return hits;
}

phazite::stretch::Source sourceOf (const juce::AudioBuffer<float>& audio)
{
    return { audio.getNumChannels(), rate, audio.getNumSamples(), [&audio] (juce::AudioBuffer<float>& buffer, juce::int64 start, int frames)
             {
                 buffer.clear (0, frames);

                 for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                     for (int i = 0; i < frames; ++i)
                         if (const auto at = start + i; at < audio.getNumSamples())
                             buffer.setSample (channel, i, audio.getSample (channel % audio.getNumChannels(), (int) at));
             } };
}

std::vector<float> render (const juce::AudioBuffer<float>& audio, const phazite::stretch::Settings& settings)
{
    std::vector<float> out;
    CHECK (phazite::stretch::render (sourceOf (audio), settings,
                                     [&out] (const juce::AudioBuffer<float>& block, int frames)
                                     {
                                         out.insert (out.end(), block.getReadPointer (0), block.getReadPointer (0) + frames);
                                         return true;
                                     }));
    CHECK ((juce::int64) out.size() == phazite::stretch::getStretchedLength (audio.getNumSamples(), settings));
    return out;
}

// Each hit's copies in what came out: where (in milliseconds from where it should be) and how strong (1 for the hit as it
// was), from correlating with its first 10 ms within 60 ms of where it should be.
struct Copy
{
    double ms, strength;
};

std::vector<std::vector<Copy>> findCopies (const Hits& hits, const std::vector<float>& out, double timeRatio)
{
    constexpr int length = 441;
    const int window = (int) (0.06 * rate);
    const auto* in = hits.audio.getReadPointer (0);
    std::vector<std::vector<Copy>> found;

    for (auto start : hits.starts)
    {
        const auto expected = (int) std::lround (start * timeRatio);

        if (start + length > hits.audio.getNumSamples() || expected + window + length > (int) out.size())
            break;

        double energy = 0.0;

        for (int i = 0; i < length; ++i)
            energy += (double) in[start + i] * in[start + i];

        const auto from = std::max (0, expected - window);
        std::vector<double> strengths;

        for (int lag = from; lag < expected + window; ++lag)
        {
            double dot = 0.0;

            for (int i = 0; i < length; ++i)
                dot += (double) out[(size_t) (lag + i)] * in[start + i];

            strengths.push_back (dot / energy);
        }

        std::vector<Copy> copies;

        for (size_t j = 1; j + 1 < strengths.size(); ++j)
            if (strengths[j] >= 0.3 && strengths[j] >= strengths[j - 1] && strengths[j] > strengths[j + 1])
                copies.push_back ({ ((double) (from + (int) j) - expected) / rate * 1000.0, strengths[j] });

        std::sort (copies.begin(), copies.end(), [] (const Copy& a, const Copy& b) { return a.strength > b.strength; });
        found.push_back (copies);
    }

    return found;
}

// The loudest millisecond (RMS) between two frames.
double loudestMillisecond (const float* samples, int from, int to)
{
    constexpr int ms = 44;
    double loudest = 0.0;

    for (int i = std::max (0, from); i + ms <= to; i += 4)
    {
        double sum = 0.0;

        for (int j = 0; j < ms; ++j)
            sum += (double) samples[i + j] * samples[i + j];

        loudest = std::max (loudest, std::sqrt (sum / ms));
    }

    return loudest;
}

// How much of a stretched chord's energy lies between its notes' partials, in decibels of all of it.
constexpr double chordNotes[] = { 110.0, 138.59, 164.81, 220.0 };

juce::AudioBuffer<float> makeChord (double seconds)
{
    juce::AudioBuffer<float> chord (1, (int) std::lround (seconds * rate));
    chord.clear();

    for (auto f : chordNotes)
        for (int h = 1; h <= 4; ++h)
            for (int i = 0; i < chord.getNumSamples(); ++i)
                chord.addSample (0, i, (float) (0.08 / h * std::sin (juce::MathConstants<double>::twoPi * f * h * i / rate + h)));

    return chord;
}

double energyBetweenNotesDb (const std::vector<float>& out)
{
    constexpr int order = 15, size = 1 << order;
    REQUIRE ((int) out.size() >= size);
    const auto start = (int) out.size() / 2 - size / 2;
    std::vector<float> data (2 * size, 0.0f);

    for (int i = 0; i < size; ++i)
        data[(size_t) i] = out[(size_t) (start + i)] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (data.data());
    double total = 0.0, between = 0.0;

    for (int bin = 1; bin < size / 2; ++bin)
    {
        const auto power = (double) data[(size_t) bin] * data[(size_t) bin];
        const auto frequency = bin * rate / size;
        total += power;
        bool onANote = false;

        for (auto f : chordNotes)
            for (int h = 1; h <= 4; ++h)
                onANote = onANote || std::abs (frequency - f * h) < 6.0;

        if (! onANote)
            between += power;
    }

    return juce::Decibels::gainToDecibels (std::sqrt (std::max (1.0e-12, between / total)), -120.0);
}

// The loudest frequency in a sound, in hertz, from the middle of it.
double loudestFrequency (const std::vector<float>& out)
{
    constexpr int order = 14, size = 1 << order;
    REQUIRE ((int) out.size() >= size);
    const auto start = (int) out.size() / 2 - size / 2;
    std::vector<float> data (2 * size, 0.0f);

    for (int i = 0; i < size; ++i)
        data[(size_t) i] = out[(size_t) (start + i)] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (data.data());
    const auto loudest = std::max_element (data.begin() + 1, data.begin() + size / 2) - data.begin();
    return (double) loudest * rate / size;
}
} // namespace

TEST_CASE ("A drum loop stretched to another tempo plays every hit once, on its beat")
{
    // Snappy hits a sixteenth apart at 120 BPM, which SoundTouch played a third of twice when slowed to 90 BPM.
    const auto hits = makeHits (3.0, 0.125, 0.03);

    const auto* in = hits.audio.getReadPointer (0);

    for (const auto songTempo : { 90.0, 110.0, 140.0 })
    {
        CAPTURE (songTempo);
        const auto timeRatio = 120.0 / songTempo;
        const auto out = render (hits.audio, { timeRatio, 0.0, false });
        const auto copies = findCopies (hits, out, timeRatio);
        REQUIRE (copies.size() >= 16);
        std::vector<double> offsets;

        for (size_t hit = 0; hit < copies.size(); ++hit)
        {
            CAPTURE (hit);

            // Once: no copy of it a millisecond or more from the strongest. (R3 makes each attack anew, rather than
            // copying it as SoundTouch does, so its copy can fall short of what counts as one.)
            for (size_t c = 1; c < copies[hit].size(); ++c)
                CHECK (std::abs (copies[hit][c].ms - copies[hit][0].ms) < 1.0);

            if (! copies[hit].empty())
                offsets.push_back (copies[hit][0].ms);

            // On its beat, within 3 ms of it, and not lost: no more than half as loud as it was (a hit SoundTouch lost was
            // 20 dB or more down; R3, sped up, softens one now and then by about 3 dB).
            const auto start = hits.starts[hit];
            const auto expected = (int) std::lround (start * timeRatio);
            const auto original = loudestMillisecond (in, start, start + 220);
            const auto stretched = loudestMillisecond (out.data(), expected - 132, expected + 132);
            CHECK (juce::Decibels::gainToDecibels (stretched / original) > -6.0);
            CHECK (juce::Decibels::gainToDecibels (stretched / original) < 6.0);
        }

        // Where a copy shows, half of them within a millisecond of their beats.
        REQUIRE (offsets.size() >= copies.size() / 2);
        std::sort (offsets.begin(), offsets.end());
        CHECK (std::abs (offsets[offsets.size() / 2]) < 1.0);
    }
}

TEST_CASE ("A held chord stretched to another tempo stays clean between its notes")
{
    // SoundTouch left -6 to -10 dB of the chord's energy between its notes; Rubber Band's R3, -16 to -29.
    const auto chord = makeChord (2.0);
    const std::vector<float> original (chord.getReadPointer (0), chord.getReadPointer (0) + chord.getNumSamples());
    CHECK (energyBetweenNotesDb (original) < -40.0); // what the measure itself leaks

    for (const auto songTempo : { 90.0, 140.0 })
    {
        CAPTURE (songTempo);
        CHECK (energyBetweenNotesDb (render (chord, { 120.0 / songTempo, 0.0, false })) < -15.0);
    }
}

TEST_CASE ("Pitch moves by semitones, the sound keeping its length")
{
    juce::AudioBuffer<float> tone (1, (int) rate);

    for (int i = 0; i < tone.getNumSamples(); ++i)
        tone.setSample (0, i, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate));

    CHECK (loudestFrequency (render (tone, { 1.0, 12.0, false })) == doctest::Approx (880.0).epsilon (0.01));
    CHECK (loudestFrequency (render (tone, { 1.0, -5.0, false })) == doctest::Approx (440.0 * std::pow (2.0, -5.0 / 12.0)).epsilon (0.01));

    // Stretched at the same time, it lasts as asked.
    CHECK (render (tone, { 1.5, 3.0, false }).size() == (size_t) (1.5 * rate));
}

TEST_CASE ("Backwards a sound plays its last frame first, and with nothing else changed is otherwise the same")
{
    juce::AudioBuffer<float> ramp (2, 10000);

    for (int i = 0; i < ramp.getNumSamples(); ++i)
    {
        ramp.setSample (0, i, (float) i / 10000.0f);
        ramp.setSample (1, i, -(float) i / 10000.0f);
    }

    const phazite::stretch::Settings backwards { 1.0, 0.0, true };
    CHECK_FALSE (phazite::stretch::isUnchanged (backwards));
    CHECK (phazite::stretch::isUnchanged ({}));

    juce::AudioBuffer<float> out (2, ramp.getNumSamples());
    int written = 0;
    CHECK (phazite::stretch::render (sourceOf (ramp), backwards,
                                     [&] (const juce::AudioBuffer<float>& block, int frames)
                                     {
                                         for (int channel = 0; channel < 2; ++channel)
                                             out.copyFrom (channel, written, block, channel, 0, frames);

                                         written += frames;
                                         return true;
                                     }));
    REQUIRE (written == ramp.getNumSamples());

    for (int i = 0; i < ramp.getNumSamples(); i += 997)
    {
        CHECK (out.getSample (0, i) == doctest::Approx (ramp.getSample (0, ramp.getNumSamples() - 1 - i)));
        CHECK (out.getSample (1, i) == doctest::Approx (ramp.getSample (1, ramp.getNumSamples() - 1 - i)));
    }
}

TEST_CASE ("A render stops when asked, or when what takes it says so")
{
    const auto chord = makeChord (1.0);
    int blocks = 0;
    CHECK_FALSE (phazite::stretch::render (
        sourceOf (chord), { 1.25, 0.0, false }, [] (const juce::AudioBuffer<float>&, int) { return true; },
        [&blocks] { return ++blocks > 2; }));
    CHECK_FALSE (
        phazite::stretch::render (sourceOf (chord), { 1.25, 0.0, false }, [] (const juce::AudioBuffer<float>&, int) { return false; }));
    CHECK (phazite::stretch::getStretchedLength (44100, { 1.5, 0.0, false }) == 66150);
}
