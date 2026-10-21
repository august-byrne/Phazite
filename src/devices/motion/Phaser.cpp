// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Phaser.h"
#include "devices/dsp/NoteValues.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view notchCounts[] = { "1", "2", "3", "4", "5", "6" };
    constexpr std::string_view shapes[] = { "Sine", "Triangle" };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "notches", .name = "Notches", .maximum = 5.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = notchCounts },
        { .id = "frequency",
          .name = "Frequency",
          .minimum = 50.0f,
          .maximum = 10000.0f,
          .defaultValue = 800.0f,
          .unit = Unit::hertz,
          .midpoint = 700.0f },
        { .id = "depth", .name = "Depth", .defaultValue = 0.6f, .unit = Unit::percent },
        { .id = "sync", .name = "Sync", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "division",
          .name = "Division",
          .maximum = (float) std::size (dsp::noteValueNames) - 1.0f,
          .defaultValue = dsp::barNoteValue,
          .unit = Unit::choice,
          .choices = dsp::noteValueNames },
        { .id = "rate", .name = "Rate", .minimum = 0.05f, .maximum = 20.0f, .defaultValue = 0.5f, .unit = Unit::hertz, .midpoint = 1.0f },
        { .id = "shape", .name = "Shape", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = shapes },
        { .id = "feedback", .name = "Feedback", .minimum = -0.85f, .maximum = 0.85f, .defaultValue = 0.3f, .unit = Unit::percent },
        { .id = "spacing", .name = "Spacing", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "stereo", .name = "Stereo", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "safeBass",
          .name = "Safe Bass",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 150.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: how far it sweeps, and how much it rings.
    constexpr std::string_view keyControls[] = { "depth", "feedback" };

    constexpr float lowestCentre = 20.0f;
    constexpr float sweepOctaves = 2.0f; // each way, at full depth

    int getNotches (std::span<const float> values)
    {
        return std::clamp ((int) std::lround (values[Phaser::notches]) + 1, 1, Phaser::maxNotches);
    }

    // Each all-pass section's Q: at 50 % spacing a half (two first-order stages alike, the classic), wider apart or closer
    // together either side.
    double getQ (std::span<const float> values)
    {
        return 0.5 * std::pow (4.0, 1.0 - 2.0 * (double) values[Phaser::spacing]);
    }

    // The sound and its phased copy, half and half, as the classic phasers mix them: where the copy is in phase (below and
    // above the sweep, and between the notches) the sound keeps its level without feedback, and where it is upside down,
    // the notches, nothing is left. Feedback then raises peaks over that, as a filter's resonance does.
    constexpr float outputGain = 0.5f;
} // namespace

const DeviceInfo Phaser::info { .typeID = typeID,
                                .name = "Phaser",
                                .kind = DeviceKind::effect,
                                .parameters = parameters,
                                .keyControls = keyControls };

double Phaser::getSweptCentre (std::span<const float> values, float lfoValue, double rate)
{
    const auto centre = values[frequency] * std::exp2 (sweepOctaves * values[depth] * std::clamp (lfoValue, -1.0f, 1.0f));
    return std::clamp ((double) centre, (double) lowestCentre, 0.45 * rate);
}

double Phaser::getMagnitude (std::span<const float> values, double centre, double frequency, double rate)
{
    // y = output (x + a), with a = A (x + feedback a): the loop solved at once, as in process().
    const auto section = dsp::makeFilter (dsp::FilterShape::allPass, centre, rate, getQ (values));
    const auto one = dsp::getResponse (section, frequency, rate);
    std::complex<double> stages (1.0, 0.0);

    for (int i = 0; i < getNotches (values); ++i)
        stages *= one;

    const auto g = (double) values[feedback];
    auto response = (double) outputGain * (1.0 + stages / (1.0 - g * stages));

    // Safe Bass: below it, the sound as it was (summed back, as process() does).
    if (values[safeBass] > lowestCentre + 0.5f)
    {
        const auto low =
            dsp::getResponse (dsp::makeFilter (dsp::FilterShape::lowPass, values[safeBass], rate, 0.70710678), frequency, rate);
        response = low + (1.0 - low) * response;
    }

    return std::abs (response);
}

void Phaser::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    lfo.prepare (sampleRate);
    reset();
}

void Phaser::reset()
{
    for (auto& channel : sections)
        for (auto& section : channel)
            section.reset();

    for (auto& low : lowParts)
        low.reset();

    lfo.reset();
}

double Phaser::getTailSeconds() const
{
    // The notches ring a little, more with feedback: at most about a second.
    return 0.05 + 1.0 * std::abs ((double) shownFeedback.load (std::memory_order_relaxed));
}

void Phaser::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    const auto numSections = getNotches (values);
    const auto q = getQ (values);
    const auto g = values[feedback];
    const auto lfoShape = values[shape] > 0.5f ? dsp::SyncedLfo::Shape::triangle : dsp::SyncedLfo::Shape::sine;
    const auto offset = 0.5 * (double) values[stereo]; // up to half a cycle, the sides opposite
    const auto keepsBass = values[safeBass] > lowestCentre + 0.5f;
    const auto lowCoefficients = dsp::makeFilter (dsp::FilterShape::lowPass, values[safeBass], sampleRate, 0.70710678);
    shownFeedback.store (g, std::memory_order_relaxed);

    lfo.startBlock (values[sync] > 0.5f, values[rate], dsp::getNoteValueBeats (values[division]), block.tempo, block.isPlaying, block.beat);

    for (int start = 0; start < block.numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, block.numSamples - start);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto centre = getSweptCentre (values, lfo.valueAt (lfoShape, channel == 1 ? offset : 0.0), sampleRate);
            const auto coefficients = dsp::makeFilter (dsp::FilterShape::allPass, centre, sampleRate, q);
            auto& chain = sections[(size_t) channel];
            auto* samples = block.channels[channel] + start;

            if (start == 0)
                centres[(size_t) channel].set ((float) centre);

            for (int i = 0; i < length; ++i)
            {
                // Safe Bass: what is below it goes around the effect, and back in after it.
                const auto x = samples[i];
                const auto low = keepsBass ? lowParts[(size_t) channel].process (x, lowCoefficients) : 0.0f;
                const auto in = x - low;

                // What comes out of the sections goes back in at once: they will put out gain * w + offset for what goes
                // in, w = in + g * that, so the loop is solved for this sample before they take it.
                auto gain = 1.0f, instantOffset = 0.0f;

                for (int s = 0; s < numSections; ++s)
                {
                    const auto instant = chain[(size_t) s].getInstant (coefficients);
                    instantOffset = instant.gain * instantOffset + instant.offset;
                    gain *= instant.gain;
                }

                auto y = in + g * (gain * in + instantOffset) / (1.0f - g * gain);

                for (int s = 0; s < numSections; ++s)
                    y = chain[(size_t) s].process (y, coefficients);

                samples[i] = low + outputGain * (in + y);
            }
        }

        lfo.advance (length);
    }

    shownPhase.set ((float) lfo.getPhase());

    if (numChannels < 2)
        centres[1].set (centres[0].get());
}
} // namespace phazite::devices
