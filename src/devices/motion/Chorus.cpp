// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Chorus.h"
#include "devices/dsp/NoteValues.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace phazite::devices
{
namespace
{
    using Lfo = dsp::SyncedLfo;

    constexpr std::string_view modes[] = { "Classic", "Ensemble" };
    constexpr std::string_view shapes[] = { "Sine", "Triangle" }; // as the phaser's and the flanger's

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "mode", .name = "Mode", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = modes },
        { .id = "delay",
          .name = "Delay",
          .minimum = 1.0f,
          .maximum = 25.0f,
          .defaultValue = 3.5f,
          .unit = Unit::milliseconds,
          .midpoint = 6.0f },
        { .id = "depth", .name = "Depth", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "sync", .name = "Sync", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "division",
          .name = "Division",
          .maximum = (float) std::size (dsp::noteValueNames) - 1.0f,
          .defaultValue = dsp::barNoteValue,
          .unit = Unit::choice,
          .choices = dsp::noteValueNames },
        { .id = "rate", .name = "Rate", .minimum = 0.05f, .maximum = 20.0f, .defaultValue = 0.5f, .unit = Unit::hertz, .midpoint = 1.0f },
        { .id = "shape", .name = "Shape", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = shapes },
        { .id = "feedback", .name = "Feedback", .maximum = 0.9f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "stereo", .name = "Stereo", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 5000.0f },
        { .id = "safeBass",
          .name = "Safe Bass",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 150.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 0.5f, .unit = Unit::percent },
    };

    // Ensemble's fast sine: ten times the rate, a whole number of times, so that it goes on smoothly as the slow LFO's cycle
    // starts again; and a tenth as deep, so that it wavers the pitch about as much as the slow one does.
    constexpr double wobbleRatio = 10.0;
    constexpr float wobbleShare = 0.1f;

    constexpr float lowestBass = 20.0f;          // Safe Bass's lowest: off
    constexpr float highestCut = 20000.0f;       // High Cut's highest: off
    constexpr double longestMilliseconds = 50.0; // the longest Delay, swept as far as Depth goes
    constexpr double gainGlideSeconds = 0.01;    // a copy fading in or out as the mode changes
    constexpr float shortestSamples = 2.0f;      // the least a copy lags: what reading the line between samples needs

    bool isEnsemble (std::span<const float> values)
    {
        return values[Chorus::mode] > 0.5f;
    }

    bool keepsBass (std::span<const float> values)
    {
        return values[Chorus::safeBass] > lowestBass + 0.5f;
    }

    bool cutsHighs (std::span<const float> values)
    {
        return values[Chorus::highCut] < highestCut - 0.5f;
    }

    dsp::FilterCoefficients makeBassFilter (std::span<const float> values, double sampleRate)
    {
        return dsp::makeFilter (dsp::FilterShape::lowPass, values[Chorus::safeBass], sampleRate, 0.70710678);
    }

    dsp::FilterCoefficients makeCutFilter (std::span<const float> values, double sampleRate)
    {
        return dsp::makeFilter (dsp::FilterShape::lowPass, std::min ((double) values[Chorus::highCut], 0.45 * sampleRate), sampleRate,
                                0.70710678);
    }

    double getFeedback (std::span<const float> values)
    {
        return std::clamp ((double) values[Chorus::feedback], 0.0, 0.9);
    }

    // Each copy's share of what comes out: as loud, with its copies together, as the sound that went in, over most of the
    // spectrum (where they don't line up); and with feedback, its peaks rising by as many decibels as what lies between them
    // falls.
    float getOutGain (std::span<const float> values)
    {
        const auto feedback = getFeedback (values);
        return (float) (std::sqrt (1.0 - feedback * feedback) / std::sqrt ((double) Chorus::getNumVoices (values)));
    }

    // And of what goes back into the line: the copies' average, so that the loop never gains.
    float getLoopGain (std::span<const float> values)
    {
        return (float) (getFeedback (values) / Chorus::getNumVoices (values));
    }
} // namespace

const DeviceInfo Chorus::info { .typeID = typeID, .name = "Chorus", .kind = DeviceKind::effect, .parameters = parameters };

int Chorus::getNumVoices (std::span<const float> values)
{
    return isEnsemble (values) ? mostVoices : 1;
}

double Chorus::getVoicePhase (std::span<const float> values, double lfoPhase, int channel, int voice)
{
    // The right side up to half a cycle ahead, the sides opposite; each copy a third of a cycle after the one before (in
    // Classic only the first plays, so that the mode's change fades the others in or out where they are).
    const auto side = channel == 1 ? 0.5 * (double) values[stereo] : 0.0;
    return lfoPhase + side + (double) voice / mostVoices;
}

float Chorus::getSweep (std::span<const float> values, double phase)
{
    const auto slow = Lfo::getValue (values[shape] > 0.5f ? Lfo::Shape::triangle : Lfo::Shape::sine, phase);

    if (! isEnsemble (values))
        return slow;

    const auto fast = Lfo::getValue (Lfo::Shape::sine, phase * wobbleRatio);
    return (slow + wobbleShare * fast) / (1.0f + wobbleShare);
}

double Chorus::getVoiceDelay (std::span<const float> values, double phase)
{
    return (double) values[delay] * (1.0 + (double) values[depth] * (double) getSweep (values, phase));
}

double Chorus::getMagnitude (std::span<const float> values, std::span<const double> milliseconds, double frequency, double rate)
{
    if (milliseconds.empty())
        return 1.0;

    // The copies' average, each a delay behind.
    std::complex<double> average;

    for (const auto each : milliseconds)
    {
        const auto samples = std::max ((double) shortestSamples, each * 0.001 * rate);
        average += std::polar (1.0, -juce::MathConstants<double>::twoPi * frequency * samples / rate);
    }

    average /= (double) milliseconds.size();

    // What goes into the line, High Cut darkening it with what the copies feed back; then the copies together.
    const auto cut = cutsHighs (values) ? dsp::getResponse (makeCutFilter (values, rate), frequency, rate) : std::complex<double> (1.0);
    const auto fedBack = getFeedback (values);
    const auto line = cut / (1.0 - fedBack * cut * average);
    auto wet = (double) getOutGain (values) * (double) milliseconds.size() * average * line;

    // Safe Bass: below it, the sound as it was.
    if (keepsBass (values))
    {
        const auto low = dsp::getResponse (makeBassFilter (values, rate), frequency, rate);
        wet = low + (1.0 - low) * wet;
    }

    const auto mixed = (double) values[mix];
    return std::abs ((1.0 - mixed) + mixed * wet);
}

void Chorus::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    gainStep = (float) std::min (1.0, controlStep / (gainGlideSeconds * sampleRate));
    lfo.prepare (sampleRate);

    const auto longest = (int) std::ceil (longestMilliseconds * 0.001 * sampleRate) + 4;
    longestSamples = (float) longest;

    for (auto& side : sides)
        side.line.prepare (longest);

    reset();
}

void Chorus::reset()
{
    for (auto& side : sides)
    {
        side.line.reset();
        side.cut.reset();
        side.low.reset();
    }

    lfo.reset();
    fresh = true;
}

void Chorus::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    const auto numVoices = getNumVoices (values);
    const auto outWanted = getOutGain (values), loopWanted = getLoopGain (values);
    const auto bassKept = keepsBass (values), cutting = cutsHighs (values);
    const auto bassFilter = makeBassFilter (values, sampleRate);
    const auto cutFilter = makeCutFilter (values, sampleRate);

    // How long it rings: its longest copy, gone round as often as its feedback takes to fall by 60 dB.
    const auto fedBack = getFeedback (values);
    const auto longest = (double) values[delay] * (1.0 + (double) values[depth]) * 0.001;
    const auto rounds = fedBack > 0.001 ? 3.0 / std::log10 (1.0 / fedBack) : 0.0;
    tailSeconds.store (longest * (1.0 + rounds) + 0.01, std::memory_order_relaxed);

    lfo.startBlock (values[sync] > 0.5f, values[rate], dsp::getNoteValueBeats (values[division]), block.tempo, block.isPlaying, block.beat);

    for (int start = 0; start < block.numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, block.numSamples - start);
        const auto lfoPhase = lfo.getPhase();

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& side = sides[(size_t) channel];
            std::array<float, mostVoices> target {};
            auto playing = 0; // the copies that play or fade, from the first

            for (int voice = 0; voice < mostVoices; ++voice)
            {
                const auto v = (size_t) voice;
                const auto milliseconds = getVoiceDelay (values, getVoicePhase (values, lfoPhase, channel, voice));
                target[v] = std::clamp ((float) (milliseconds * 0.001 * sampleRate), shortestSamples, longestSamples);
                const auto on = voice < numVoices;
                const auto outTarget = on ? outWanted : 0.0f, loopTarget = on ? loopWanted : 0.0f;

                if (fresh)
                {
                    side.now[v] = target[v];
                    side.outGain[v] = outTarget;
                    side.loopGain[v] = loopTarget;
                }
                else
                {
                    side.outGain[v] += std::clamp (outTarget - side.outGain[v], -gainStep, gainStep);
                    side.loopGain[v] += std::clamp (loopTarget - side.loopGain[v], -gainStep, gainStep);
                }

                side.step[v] = (target[v] - side.now[v]) / (float) length;

                if (side.outGain[v] > 0.0f || side.loopGain[v] > 0.0f)
                    playing = voice + 1;

                if (start == 0)
                    delays[(size_t) channel][v].set ((float) milliseconds);
            }

            auto* samples = block.channels[channel] + start;

            for (int i = 0; i < length; ++i)
            {
                // Safe Bass: what is below it goes around the copies, and back in after them.
                const auto x = samples[i];
                const auto low = bassKept ? side.low.process (x, bassFilter) : 0.0f;
                auto out = 0.0f, back = 0.0f;

                // Each copy a delay behind what goes in now (one sample less, from the last one in).
                for (int voice = 0; voice < playing; ++voice)
                {
                    const auto v = (size_t) voice;
                    side.now[v] += side.step[v];
                    const auto copy = side.line.read (side.now[v] - 1.0f);
                    out += side.outGain[v] * copy;
                    back += side.loopGain[v] * copy;
                }

                const auto in = x - low + back;
                side.line.push (cutting ? side.cut.process (in, cutFilter) : in);
                samples[i] = low + out;
            }

            side.now = target;
        }

        fresh = false;
        lfo.advance (length);
    }

    shownPhase.set ((float) lfo.getPhase());

    if (numChannels < 2)
        for (size_t voice = 0; voice < (size_t) mostVoices; ++voice)
            delays[1][voice].set (delays[0][voice].get());
}
} // namespace phazite::devices
