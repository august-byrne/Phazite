// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Container.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
ContainerState::ContainerState (int maximumBlockSize, int samples)
    : latency (std::max (0, samples)),
      dry (2, std::max (1, maximumBlockSize)),
      delay (std::make_unique<Delay> (samples))
{
    dry.clear();
}

void ContainerState::setMix (float mix) noexcept
{
    mixValue.store (juce::jlimit (0.0f, 1.0f, mix), std::memory_order_relaxed);
}

void ContainerState::keepDry (const float* const* channels, int numSamples) noexcept
{
    // What comes in, delayed as much as the container's chains delay their sound.
    float* kept[] = { dry.getWritePointer (0), dry.getWritePointer (1) };
    juce::FloatVectorOperations::clear (kept[0], numSamples);
    juce::FloatVectorOperations::clear (kept[1], numSamples);
    delay->addDelayed (channels, kept, numSamples);
}

void ContainerState::mix (float* const* channels, int numSamples) noexcept
{
    // The chains' sound by the mix, what came in by the rest of it: a straight line from the last block's mix.
    const auto now = mixValue.load (std::memory_order_relaxed);
    const auto from = applied < 0.0f ? now : applied;
    applied = now;

    if (juce::exactlyEqual (from, 1.0f) && juce::exactlyEqual (now, 1.0f))
        return;

    const auto step = (now - from) / (float) numSamples;

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto* kept = dry.getReadPointer (channel);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto wet = from + step * (float) i;
            channels[channel][i] = channels[channel][i] * wet + kept[i] * (1.0f - wet);
        }
    }
}

void ContainerState::reset() noexcept
{
    dry.clear();

    if (delay != nullptr)
        delay->reset();
}

//==============================================================================
void ChainState::prepare (double rate, int maximumBlockSize)
{
    sampleRate = rate;
    buffer.setSize (4, std::max (1, maximumBlockSize));
    buffer.clear();
    const auto points = (size_t) (maximumBlockSize / controlPointSpacing + 3);
    volumeModulation.assign (points, 0.0f);
    panModulation.assign (points, 0.0f);
    crossoverModulation.assign (points, 0.0f);
    fader.prepare (rate);
    crossoverRange = devices::getRange (crossoverInfo);
    madeFor = -1.0f;
    wasSilenced = false;
    reset();
}

void ChainState::setNotes (int lowestKey, int highestKey, int lowestVelocity, int highestVelocity) noexcept
{
    lowKey.store (lowestKey, std::memory_order_relaxed);
    highKey.store (highestKey, std::memory_order_relaxed);
    lowVelocity.store (lowestVelocity, std::memory_order_relaxed);
    highVelocity.store (highestVelocity, std::memory_order_relaxed);
}

float ChainState::findCrossover (const Curve* automation, double beat, std::span<const float> modulation, size_t point) noexcept
{
    // Its automation's, or its value set by hand; then moved by its modulation, in its knob's travel.
    const auto base =
        automation != nullptr && ! crossoverWriting.load (std::memory_order_relaxed) ? automation->getValueAt (beat) : getCrossover();
    auto value = juce::jlimit (crossoverInfo.minimum, crossoverInfo.maximum, base);
    playedBaseCrossover.store (value, std::memory_order_relaxed);

    if (! modulation.empty())
        value = crossoverRange.convertFrom0to1 (
            juce::jlimit (0.0f, 1.0f, crossoverRange.convertTo0to1 (value) + modulation[std::min (point, modulation.size() - 1)]));

    playedCrossover.store (value, std::memory_order_relaxed);
    return value;
}

bool ChainState::takes (const devices::NoteEvent& event) noexcept
{
    using Type = devices::NoteEvent::Type;

    switch (event.type)
    {
        case Type::noteOn:
        case Type::portamentoOn:
        {
            // In its ranges, by the note's velocity as a clip has it (1 to 127).
            const auto pitch = std::clamp (event.number, 0, 127);
            const auto velocity = std::clamp ((int) std::lround (event.value * 127.0f), 1, 127);

            if (pitch < lowKey.load (std::memory_order_relaxed) || pitch > highKey.load (std::memory_order_relaxed)
                || velocity < lowVelocity.load (std::memory_order_relaxed) || velocity > highVelocity.load (std::memory_order_relaxed))
                return false;

            sounding.set ((size_t) pitch);
            return true;
        }

        case Type::noteOff:
        {
            // Wherever its range is now: a note ends in the chain it started in.
            const auto pitch = (size_t) std::clamp (event.number, 0, 127);

            if (! sounding.test (pitch))
                return false;

            sounding.reset (pitch);
            return true;
        }

        case Type::allNotesOff:
            sounding.reset();
            return true;

        case Type::pitchBend:
        case Type::controller:
        case Type::slide:
            break;
    }

    return true;
}

void ChainState::reset() noexcept
{
    sounding.reset();
    numEvents = 0;

    for (auto& side : split)
        for (auto& filter : side)
            filter.reset();

    for (auto& side : phase)
        for (auto& filter : side)
            filter.reset();
}

//==============================================================================
namespace
{
    // The sound split into bands at the chains' crossovers: from the bottom, each crossover splits what is above the one
    // before it, its low side the band below it; and each band below the last two passes through the all-passes of the
    // crossovers above the one that made it, as the bands above it went through them, so that all of them add up to what came
    // in at every frequency (through those all-passes).
    void splitBands (const float* const* input, int numSamples, std::span<const ChainPart> chains, size_t count, float* const* lefts,
                     float* const* rights, const SplitPosition& position) noexcept
    {
        using devices::dsp::FilterShape;
        constexpr double butterworthQ = 0.70710678;
        const auto spacing = (std::int64_t) controlPointSpacing;
        const auto toControlPoint = (int) (spacing - (((position.sample % spacing) + spacing) % spacing));

        for (int done = 0, stretch = 0; done < numSamples; ++stretch)
        {
            // Each crossover where it is on the next control point, none below the one before it, its sections made anew
            // where it moved.
            const auto toPoint = done == 0 ? toControlPoint : (int) spacing;
            const auto length = std::min (numSamples - done, toPoint);
            const auto beat = position.beat + (double) (done + toPoint) * position.beatsPerSample;
            auto below = crossoverInfo.minimum;

            for (size_t k = 1; k < count; ++k)
            {
                auto& state = *chains[k].state;
                const auto highest = (float) (0.45 * state.sampleRate);
                auto frequency = state.findCrossover (chains[k].crossover, beat, chains[k].crossoverModulation, (size_t) stretch + 1);
                frequency = std::min (std::max (frequency, below), highest);
                below = frequency;

                if (! juce::exactlyEqual (frequency, state.madeFor))
                {
                    state.low = devices::dsp::makeFilter (FilterShape::lowPass, frequency, state.sampleRate, butterworthQ);
                    state.high = devices::dsp::makeFilter (FilterShape::highPass, frequency, state.sampleRate, butterworthQ);
                    state.allPass = devices::dsp::makeFilter (FilterShape::allPass, frequency, state.sampleRate, butterworthQ);
                    state.madeFor = frequency;
                }
            }

            for (size_t side = 0; side < 2; ++side)
            {
                auto* const* outputs = side == 0 ? lefts : rights;

                for (int i = done; i < done + length; ++i)
                {
                    auto rest = input[side][i];

                    for (size_t k = 1; k < count; ++k)
                    {
                        auto& state = *chains[k].state;
                        auto& filters = state.split[side];
                        outputs[k - 1][i] = filters[1].process (filters[0].process (rest, state.low), state.low);
                        rest = filters[3].process (filters[2].process (rest, state.high), state.high);
                    }

                    outputs[count - 1][i] = rest;

                    for (size_t band = 0; band + 2 < count; ++band)
                    {
                        auto value = outputs[band][i];
                        auto& phase = chains[band].state->phase[side];

                        for (size_t k = band + 2; k < count; ++k)
                            value = phase[k].process (value, chains[k].state->allPass);

                        outputs[band][i] = value;
                    }
                }
            }

            done += length;
        }
    }
} // namespace

void splitIntoParts (Split split, const float* const* input, int offset, int numSamples, std::span<const ChainPart> chains,
                     const SplitPosition& position) noexcept
{
    const auto count = std::min (chains.size(), (size_t) maximumChains);
    std::array<float*, maximumChains> lefts {}, rights {};

    for (size_t c = 0; c < count; ++c)
    {
        lefts[c] = chains[c].state->buffer.getWritePointer (0, offset);
        rights[c] = chains[c].state->buffer.getWritePointer (1, offset);
    }

    if (count == 0)
        return;

    switch (split)
    {
        case Split::layers:
            // Each chain all of it.
            for (size_t c = 0; c < count; ++c)
            {
                juce::FloatVectorOperations::copy (lefts[c], input[0], numSamples);
                juce::FloatVectorOperations::copy (rights[c], input[1], numSamples);
            }

            return;

        case Split::bands:
            if (count == 1)
            {
                juce::FloatVectorOperations::copy (lefts[0], input[0], numSamples);
                juce::FloatVectorOperations::copy (rights[0], input[1], numSamples);
                return;
            }

            splitBands (input, numSamples, chains, count, lefts.data(), rights.data(), position);
            return;

        case Split::midSide:
        case Split::leftRight:
            // Two parts: the middle, the same on both sides, and the side, opposite on each, which add up to the left and the
            // right; or the left alone and the right alone. A chain past them plays nothing.
            for (size_t c = 2; c < count; ++c)
            {
                juce::FloatVectorOperations::clear (lefts[c], numSamples);
                juce::FloatVectorOperations::clear (rights[c], numSamples);
            }

            for (int i = 0; i < numSamples; ++i)
            {
                const auto left = input[0][i], right = input[1][i];

                if (split == Split::midSide)
                {
                    const auto middle = 0.5f * (left + right), side = 0.5f * (left - right);
                    lefts[0][i] = middle;
                    rights[0][i] = middle;

                    if (count > 1)
                    {
                        lefts[1][i] = side;
                        rights[1][i] = -side;
                    }
                }
                else
                {
                    lefts[0][i] = left;
                    rights[0][i] = 0.0f;

                    if (count > 1)
                    {
                        lefts[1][i] = 0.0f;
                        rights[1][i] = right;
                    }
                }
            }

            return;
    }
}
} // namespace phazite::engine
