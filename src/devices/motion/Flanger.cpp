// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Flanger.h"
#include "devices/dsp/NoteValues.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view shapes[] = { "Sine", "Triangle" }; // as the phaser's

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "delay",
          .name = "Delay",
          .minimum = 0.2f,
          .maximum = 15.0f,
          .defaultValue = 3.0f,
          .unit = Unit::milliseconds,
          .midpoint = 2.0f },
        { .id = "depth", .name = "Depth", .defaultValue = 0.7f, .unit = Unit::percent },
        { .id = "sync", .name = "Sync", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "division",
          .name = "Division",
          .maximum = (float) std::size (dsp::noteValueNames) - 1.0f,
          .defaultValue = dsp::twoBarsNoteValue,
          .unit = Unit::choice,
          .choices = dsp::noteValueNames },
        { .id = "rate", .name = "Rate", .minimum = 0.05f, .maximum = 20.0f, .defaultValue = 0.25f, .unit = Unit::hertz, .midpoint = 1.0f },
        { .id = "shape", .name = "Shape", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = shapes },
        { .id = "feedback", .name = "Feedback", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.3f, .unit = Unit::percent },
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

    constexpr double leastLoss = 0.02;        // what the loop loses each time round at full feedback: 2 %
    constexpr double signGlideSeconds = 0.01; // the feedback's sign, turning from one side to the other
    constexpr double longestMilliseconds = 15.0;

    // The comb, as the comb filter's makeComb() (devices/dsp/CombFilter.h) makes it, with the copy lagging so many
    // milliseconds and the feedback's sign as given (it glides): positive, the comb rings at multiples of 1 / the delay
    // (metallic); negative, it hollows out there. Its notches are as deep at any feedback, and its peaks rise by as many
    // decibels as what lies between them falls.
    dsp::CombCoefficients makeComb (std::span<const float> values, double milliseconds, float sign, double sampleRate)
    {
        const auto loss = std::pow (leastLoss, std::clamp (std::abs ((double) values[Flanger::feedback]), 0.0, 1.0));
        dsp::CombCoefficients c;
        c.delay = (float) std::max (2.0, milliseconds * 0.001 * sampleRate);
        c.feedback = (float) ((double) sign * (1.0 - loss));
        c.input = (float) loss;
        c.output = (float) (0.5 * std::pow (loss, -0.25));
        c.sign = sign;
        return c;
    }
} // namespace

const DeviceInfo Flanger::info { .typeID = typeID, .name = "Flanger", .kind = DeviceKind::effect, .parameters = parameters };

double Flanger::getSweptDelay (std::span<const float> values, float lfoValue)
{
    // The LFO's top is the Delay; its bottom, up to five octaves shorter at full depth.
    const auto up = 0.5f * (std::clamp (lfoValue, -1.0f, 1.0f) + 1.0f);
    return (double) values[delay] * std::exp2 (-sweepOctaves * values[depth] * (1.0f - up));
}

double Flanger::getMagnitude (std::span<const float> values, double milliseconds, double frequency, double rate)
{
    const auto c = makeComb (values, milliseconds, values[feedback] < 0.0f ? -1.0f : 1.0f, rate);
    const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * frequency * (double) c.delay / rate);
    auto response = (double) c.output * (double) c.input * (1.0 + (double) c.sign * z) / (1.0 - (double) c.feedback * z);

    // Safe Bass: the comb hears the band above it, and the band below goes around it, added back after it, as process()
    // plays them (SafeBass.h).
    if (SafeBass::isOn (values[safeBass]))
    {
        const auto bands = dsp::Crossover::getResponses (values[safeBass], frequency, rate);
        response = bands.low + bands.high * response;
    }

    return std::abs (response);
}

void Flanger::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    signStep = (float) std::min (2.0, 2.0 * controlStep / (signGlideSeconds * sampleRate));
    lfo.prepare (sampleRate);
    bassSplit.prepare (sampleRate);

    for (auto& comb : combs)
        comb.prepare ((int) std::ceil (longestMilliseconds * 0.001 * sampleRate) + 4);

    reset();
}

void Flanger::reset()
{
    for (auto& comb : combs)
        comb.reset();

    bassSplit.reset();
    lfo.reset();
    sign = 1.0f;
}

void Flanger::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    const auto lfoShape = values[shape] > 0.5f ? dsp::SyncedLfo::Shape::triangle : dsp::SyncedLfo::Shape::sine;
    const auto offset = 0.5 * (double) values[stereo]; // up to half a cycle, the sides opposite
    const auto targetSign = values[feedback] < 0.0f ? -1.0f : 1.0f;
    bassSplit.startBlock (values[safeBass]);

    // How long it rings, at its longest delay.
    tailSeconds.store (dsp::getRingSeconds (makeComb (values, values[delay], targetSign, sampleRate), sampleRate) + 0.01,
                       std::memory_order_relaxed);

    lfo.startBlock (values[sync] > 0.5f, values[rate], dsp::getNoteLength (values[division]), block.tempo, block.barLength, block.isPlaying,
                    block.beat, block.bars);

    for (int start = 0; start < block.numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, block.numSamples - start);
        sign += std::clamp (targetSign - sign, -signStep, signStep);
        const auto splitting = bassSplit.startStretch (length);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto milliseconds = getSweptDelay (values, lfo.valueAt (lfoShape, channel == 1 ? offset : 0.0));
            auto& comb = combs[(size_t) channel];
            comb.setTarget (makeComb (values, milliseconds, sign, sampleRate), length);

            if (start == 0)
                delays[(size_t) channel].set ((float) milliseconds);

            auto* samples = block.channels[channel] + start;
            auto* dry = block.dry != nullptr ? block.dry[channel] + start : nullptr;

            for (int i = 0; i < length; ++i)
            {
                // Safe Bass: the band below it goes around the comb, and back in after it; the sound the runner mixes in
                // is split and added up again, to stay in phase with the two (SafeBass.h).
                const auto x = samples[i];
                auto in = x, around = 0.0f;

                if (splitting)
                {
                    const auto parts = bassSplit.split (channel, x, i);
                    in = parts.through;
                    around = parts.around;

                    if (dry != nullptr)
                        dry[i] = parts.dry;
                }

                samples[i] = around + comb.process (in);
            }
        }

        lfo.advance (length);
    }

    shownPhase.set ((float) lfo.getPhase());

    if (numChannels < 2)
        delays[1].set (delays[0].get());
}
} // namespace phazite::devices
