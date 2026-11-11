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

    constexpr std::string_view voiceCounts[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    constexpr std::string_view shapes[] = { "Sine", "Triangle" }; // as the phaser's and the flanger's

    static_assert (std::size (voiceCounts) == (size_t) Chorus::mostVoices);

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "voices",
          .name = "Voices",
          .maximum = (float) Chorus::mostVoices - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = voiceCounts },
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
        { .id = "shimmer", .name = "Shimmer", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "voicePhase", .name = "Voice Phase", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "spread", .name = "Spread", .defaultValue = 0.0f, .unit = Unit::percent },
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

    // Shimmer's fast sine: ten times the rate, a whole number of times, so that it goes on smoothly as the slow LFO's cycle
    // starts again; and at its most a tenth as deep, so that it wavers the pitch about as much as the slow one does.
    constexpr double wobbleRatio = 10.0;
    constexpr float wobbleShare = 0.1f;

    constexpr float highestCut = 20000.0f;       // High Cut's highest: off
    constexpr double longestMilliseconds = 50.0; // the longest Delay, swept as far as Depth goes
    constexpr double fadeSeconds = 0.01;         // a set of copies fading in or out as their number changes
    constexpr float shortestSamples = 2.0f;      // the least a copy lags: what reading the line between samples needs

    int getCount (std::span<const float> values)
    {
        return std::clamp ((int) std::lround (values[Chorus::voices]) + 1, 1, Chorus::mostVoices);
    }

    // How far apart in the LFO's cycle neighbouring copies are, of so many a side. At the most (Voice Phase at 100 %) the
    // copies of both sides, the right side half a cycle ahead, are spread evenly round the cycle: a quarter of it apart with
    // two a side, a third with three (the string machines' 120 degrees), three eighths with four, and so on toward a half,
    // which also spreads an odd number evenly on each side by itself.
    double getPhaseStep (std::span<const float> values, int count)
    {
        return (double) values[Chorus::voicePhase] * (double) (count - 1) / (2.0 * (double) count);
    }

    double getPhaseOf (std::span<const float> values, int count, double lfoPhase, int channel, int voice)
    {
        // The right side up to half a cycle ahead, the sides opposite.
        const auto side = channel == 1 ? 0.5 * (double) values[Chorus::stereo] : 0.0;
        return lfoPhase + side + (double) voice * getPhaseStep (values, count);
    }

    // A copy's part of the whole sweep (Spread): its middle, from -1 (the whole sweep's bottom) to 1 (its top), and how far
    // it goes each way, as a part of the whole. As MultiChorus lays its voices out, its overlap 1 - Spread: each covers
    // 1 / (1 + Spread (count - 1)) of the sweep, Spread of that further along than the copy before, so that at 100 % the
    // parts lie end to end and at none every copy has the whole sweep.
    struct Part
    {
        double middle = 0.0, reach = 1.0;
    };

    Part getPart (std::span<const float> values, int count, int voice)
    {
        const auto apart = std::clamp ((double) values[Chorus::spread], 0.0, 1.0);

        if (apart <= 0.0 || count <= 1)
            return {};

        const auto reach = 1.0 / (1.0 + apart * (double) (count - 1));
        return { (2.0 * (double) voice * apart + 1.0) * reach - 1.0, reach };
    }

    double getDelayOf (std::span<const float> values, int count, double phase, int voice)
    {
        const auto sweep = (double) Chorus::getSweep (values, phase);
        const auto part = getPart (values, count, voice);
        const auto at = part.reach < 1.0 ? part.middle + part.reach * sweep : sweep;
        return (double) values[Chorus::delay] * (1.0 + (double) values[Chorus::depth] * at);
    }

    bool cutsHighs (std::span<const float> values)
    {
        return values[Chorus::highCut] < highestCut - 0.5f;
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

    // Each copy's share of what comes out, of so many a side: as loud, with its copies together, as the sound that went in,
    // over most of the spectrum (where they don't line up), however many they are; and with feedback, its peaks rising by as
    // many decibels as what lies between them falls.
    float getOutGain (std::span<const float> values, int count)
    {
        const auto feedback = getFeedback (values);
        return (float) (std::sqrt (1.0 - feedback * feedback) / std::sqrt ((double) count));
    }

    // And of what goes back into the line: the copies' average, so that the loop never gains.
    float getLoopGain (std::span<const float> values, int count)
    {
        return (float) (getFeedback (values) / count);
    }
} // namespace

const DeviceInfo Chorus::info { .typeID = typeID, .name = "Chorus", .kind = DeviceKind::effect, .parameters = parameters };

int Chorus::getNumVoices (std::span<const float> values)
{
    return getCount (values);
}

double Chorus::getVoicePhase (std::span<const float> values, double lfoPhase, int channel, int voice)
{
    return getPhaseOf (values, getCount (values), lfoPhase, channel, voice);
}

float Chorus::getSweep (std::span<const float> values, double phase)
{
    const auto slow = Lfo::getValue (values[shape] > 0.5f ? Lfo::Shape::triangle : Lfo::Shape::sine, phase);
    const auto share = wobbleShare * std::clamp (values[shimmer], 0.0f, 1.0f);

    if (share <= 0.0f)
        return slow;

    const auto fast = Lfo::getValue (Lfo::Shape::sine, phase * wobbleRatio);
    return (slow + share * fast) / (1.0f + share);
}

double Chorus::getVoiceDelay (std::span<const float> values, double phase, int voice)
{
    return getDelayOf (values, getCount (values), phase, voice);
}

Chorus::Reach Chorus::getVoiceReach (std::span<const float> values, int voice)
{
    const auto part = getPart (values, getCount (values), voice);
    const auto middle = (double) values[delay], reach = (double) values[depth];
    return { middle * (1.0 + reach * (part.middle - part.reach)), middle * (1.0 + reach * (part.middle + part.reach)) };
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
    auto wet = (double) getOutGain (values, getCount (values)) * (double) milliseconds.size() * average * line;

    // Safe Bass: the copies hear the band above it, the band below goes around them, and the sound its runner mixes in is
    // the two added up, as process() plays them (SafeBass.h).
    std::complex<double> dry (1.0);

    if (SafeBass::isOn (values[safeBass]))
    {
        const auto bands = dsp::Crossover::getResponses (values[safeBass], frequency, rate);
        wet = bands.low + bands.high * wet;
        dry = bands.low + bands.high;
    }

    const auto mixed = (double) values[mix];
    return std::abs ((1.0 - mixed) * dry + mixed * wet);
}

void Chorus::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    fadeStep = (float) std::min (1.0, controlStep / (fadeSeconds * sampleRate));
    lfo.prepare (sampleRate);

    const auto longest = (int) std::ceil (longestMilliseconds * 0.001 * sampleRate) + 4;
    longestSamples = (float) longest;

    for (auto& side : sides)
        side.line.prepare (longest);

    bassSplit.prepare (sampleRate);
    reset();
}

void Chorus::reset()
{
    for (auto& side : sides)
    {
        side.line.reset();
        side.cut.reset();
    }

    bassSplit.reset();
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

    const auto wantedCount = getCount (values);
    const auto cutting = cutsHighs (values);
    const auto cutFilter = makeCutFilter (values, sampleRate);
    bassSplit.startBlock (values[safeBass]);

    // How long it rings: its longest copy, gone round as often as its feedback takes to fall by 60 dB.
    const auto fedBack = getFeedback (values);
    const auto longest = (double) values[delay] * (1.0 + (double) values[depth]) * 0.001;
    const auto rounds = fedBack > 0.001 ? 3.0 / std::log10 (1.0 / fedBack) : 0.0;
    tailSeconds.store (longest * (1.0 + rounds) + 0.01, std::memory_order_relaxed);

    lfo.startBlock (values[sync] > 0.5f, values[rate], dsp::getNoteLength (values[division]), block.tempo, block.barLength, block.isPlaying,
                    block.beat, block.bars);

    for (int start = 0; start < block.numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, block.numSamples - start);
        const auto lfoPhase = lfo.getPhase();
        const auto splitting = bassSplit.startStretch (length);

        // Another number of copies: a set of that many fades in where they are, as the set that played fades out where it
        // was. One change at a time: the next waits until the set before has faded out.
        auto started = false; // the front set's copies start where they are, rather than gliding there

        if (fresh)
        {
            front = 0;
            sets[0] = { .count = wantedCount, .fade = 1.0f };
            sets[1] = {};
        }
        else if (wantedCount != sets[(size_t) front].count && sets[(size_t) (1 - front)].count == 0)
        {
            front = 1 - front;
            sets[(size_t) front].count = wantedCount;
            started = true;
        }

        // Each set's copies' gains, gliding over the step to where its fade and the feedback put them: a set's copies
        // together send back the feedback's share, and two sets, while one gives way to the other, no more than one does.
        std::array<float, numSets> outFrom {}, loopFrom {}, outStep {}, loopStep {};

        for (size_t s = 0; s < (size_t) numSets; ++s)
        {
            auto& set = sets[s];

            if (set.count == 0)
                continue;

            if (! fresh)
                set.fade = std::clamp (set.fade + ((int) s == front ? fadeStep : -fadeStep), 0.0f, 1.0f);

            const auto outTo = set.fade * getOutGain (values, set.count), loopTo = set.fade * getLoopGain (values, set.count);
            outFrom[s] = fresh ? outTo : set.outGain;
            loopFrom[s] = fresh ? loopTo : set.loopGain;
            outStep[s] = (outTo - outFrom[s]) / (float) length;
            loopStep[s] = (loopTo - loopFrom[s]) / (float) length;
            set.outGain = outTo;
            set.loopGain = loopTo;
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& side = sides[(size_t) channel];
            std::array<std::array<float, mostVoices>, numSets> target {};

            for (size_t s = 0; s < (size_t) numSets; ++s)
            {
                const auto count = sets[s].count;
                const auto isFront = (int) s == front;

                for (int voice = 0; voice < count; ++voice)
                {
                    const auto v = (size_t) voice;
                    const auto milliseconds = getDelayOf (values, count, getPhaseOf (values, count, lfoPhase, channel, voice), voice);
                    target[s][v] = std::clamp ((float) (milliseconds * 0.001 * sampleRate), shortestSamples, longestSamples);

                    if (fresh || (started && isFront))
                        side.now[s][v] = target[s][v];

                    side.step[s][v] = (target[s][v] - side.now[s][v]) / (float) length;

                    if (start == 0 && isFront)
                        delays[(size_t) channel][v].set ((float) milliseconds);
                }
            }

            auto* samples = block.channels[channel] + start;
            auto* dry = block.dry != nullptr ? block.dry[channel] + start : nullptr;
            auto outGain = outFrom, loopGain = loopFrom;

            for (int i = 0; i < length; ++i)
            {
                // Safe Bass: the band below it goes around the copies, and back in after them; the sound the runner mixes
                // in is split and added up again, to stay in phase with the two (SafeBass.h).
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

                auto out = 0.0f, back = 0.0f;

                // Each copy a delay behind what goes in now (one sample less, from the last one in).
                for (size_t s = 0; s < (size_t) numSets; ++s)
                {
                    const auto count = (size_t) sets[s].count;
                    outGain[s] += outStep[s];
                    loopGain[s] += loopStep[s];

                    for (size_t v = 0; v < count; ++v)
                    {
                        side.now[s][v] += side.step[s][v];
                        const auto copy = side.line.read (side.now[s][v] - 1.0f);
                        out += outGain[s] * copy;
                        back += loopGain[s] * copy;
                    }
                }

                const auto line = in + back;
                side.line.push (cutting ? side.cut.process (line, cutFilter) : line);
                samples[i] = around + out;
            }

            for (size_t s = 0; s < (size_t) numSets; ++s)
                for (size_t v = 0; v < (size_t) sets[s].count; ++v)
                    side.now[s][v] = target[s][v];
        }

        // A set that has faded out rests.
        if (auto& behind = sets[(size_t) (1 - front)]; behind.count != 0 && behind.fade <= 0.0f)
            behind = {};

        fresh = false;
        lfo.advance (length);
    }

    shownPhase.set ((float) lfo.getPhase());

    if (numChannels < 2)
        for (size_t voice = 0; voice < (size_t) mostVoices; ++voice)
            delays[1][voice].set (delays[0][voice].get());
}
} // namespace phazite::devices
