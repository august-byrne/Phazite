// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulatorNode.h"
#include "engine/automation/Curve.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
ModulatorNode::ModulatorNode (devices::ModulatorKind kind)
    : modulator (std::in_place, kind)
{
    const auto infos = devices::getModulatorParameters (kind);
    parameters = std::make_unique<std::atomic<float>[]> (infos.size());

    for (size_t i = 0; i < infos.size(); ++i)
        parameters[i].store (infos[i].defaultValue, std::memory_order_relaxed);

    for (auto& step : steps)
        step.store (0.0f, std::memory_order_relaxed);
}

ModulatorNode::ModulatorNode()
{
    for (auto& step : steps)
        step.store (0.0f, std::memory_order_relaxed);
}

std::optional<devices::ModulatorKind> ModulatorNode::getKind() const noexcept
{
    return modulator.has_value() ? std::optional (modulator->getKind()) : std::nullopt;
}

void ModulatorNode::setParameter (int index, float value) noexcept
{
    if (modulator.has_value() && juce::isPositiveAndBelow (index, (int) devices::getModulatorParameters (modulator->getKind()).size()))
        parameters[(size_t) index].store (value, std::memory_order_relaxed);
}

void ModulatorNode::setStep (int index, float value) noexcept
{
    if (juce::isPositiveAndBelow (index, (int) steps.size()))
        steps[(size_t) index].store (value, std::memory_order_relaxed);
}

void ModulatorNode::setOn (bool isOn) noexcept
{
    on.store (isOn, std::memory_order_relaxed);
}

void ModulatorNode::setMacroValue (float value) noexcept
{
    macroValue.store (juce::jlimit (0.0f, 1.0f, value), std::memory_order_relaxed);
}

void ModulatorNode::prepare (double rate, int maximumBlockSize, std::uint32_t seed)
{
    sampleRate = rate;
    values.assign ((size_t) (maximumBlockSize / controlPointSpacing + 3), 0.0f);
    numValues = 0;
    lastIndex = noPoint;
    macroStarted = false;
    sinceHistory = 0.0;

    if (modulator.has_value())
    {
        modulator->prepare (rate, seed);
        blockParameters.assign (devices::getModulatorParameters (modulator->getKind()).size(), 0.0f);
        blockSteps.assign (steps.size(), 0.0f);
    }

    if (history == nullptr)
    {
        history = std::make_unique<std::atomic<float>[]> ((size_t) historySize);

        for (int i = 0; i < historySize; ++i)
            history[(size_t) i].store (0.0f, std::memory_order_relaxed);
    }
}

void ModulatorNode::noteOn (int pitch, int velocity) noexcept
{
    if (modulator.has_value())
        modulator->noteOn (pitch, velocity);
}

void ModulatorNode::listen (const float* const* channels, int numChannels, int numSamples) noexcept
{
    if (modulator.has_value() && modulator->getKind() == devices::ModulatorKind::envelopeFollower)
    {
        for (size_t i = 0; i < blockParameters.size(); ++i)
            blockParameters[i] = parameters[i].load (std::memory_order_relaxed);

        modulator->listen (channels, numChannels, numSamples, blockParameters);
    }
}

void ModulatorNode::play (std::span<const Point> points, std::span<const NoteStart> notes, double secondsPerPoint) noexcept
{
    numValues = (int) std::min (points.size(), values.size());

    if (numValues == 0)
        return;

    if (modulator.has_value())
    {
        for (size_t i = 0; i < blockParameters.size(); ++i)
            blockParameters[i] = parameters[i].load (std::memory_order_relaxed);

        for (size_t i = 0; i < blockSteps.size(); ++i)
            blockSteps[i] = steps[i].load (std::memory_order_relaxed);

        // Each point, after the notes started before it; one given in the last block, as it was.
        size_t nextNote = 0;

        for (int i = 0; i < numValues; ++i)
        {
            const auto& point = points[(size_t) i];

            while (nextNote < notes.size() && notes[nextNote].sample < point.sample)
            {
                modulator->noteOn (notes[nextNote].pitch, notes[nextNote].velocity);
                ++nextNote;
            }

            if (point.index == lastIndex || point.index == lastIndex - 1)
            {
                values[(size_t) i] = lastValues[point.index == lastIndex ? 1 : 0];
                continue;
            }

            values[(size_t) i] = modulator->next (blockParameters, blockSteps, point.place, secondsPerPoint);
            lastValues = { point.index == lastIndex + 1 ? lastValues[1] : values[(size_t) i], values[(size_t) i] };
            lastIndex = point.index;
        }

        for (; nextNote < notes.size(); ++nextNote)
            modulator->noteOn (notes[nextNote].pitch, notes[nextNote].velocity);
    }
    else
    {
        // A macro glides from its value as the last block ended to its value now, over the block's points.
        const auto target = macroValue.load (std::memory_order_relaxed);

        if (! macroStarted)
            lastMacro = target;

        macroStarted = true;

        for (int i = 0; i < numValues; ++i)
            values[(size_t) i] = numValues > 1 ? lastMacro + (target - lastMacro) * (float) i / (float) (numValues - 1) : target;

        lastMacro = target;
    }

    // What it played: its value now, and a hundred a second for its editor.
    now.store (values[(size_t) numValues - 1], std::memory_order_relaxed);
    sinceHistory += secondsPerPoint * (numValues - 1);

    while (sinceHistory >= 1.0 / historyRate && history != nullptr)
    {
        sinceHistory -= 1.0 / historyRate;
        const auto end = historyEnd.load (std::memory_order_relaxed);
        history[(size_t) end].store (values[(size_t) numValues - 1], std::memory_order_relaxed);
        historyEnd.store ((end + 1) % historySize, std::memory_order_release);
    }
}

std::vector<float> ModulatorNode::getHistory (double seconds) const
{
    std::vector<float> out;

    if (history == nullptr)
        return out;

    const auto count = juce::jlimit (0, historySize, (int) std::lround (seconds * historyRate));
    const auto end = historyEnd.load (std::memory_order_acquire);

    for (int i = count; i > 0; --i)
        out.push_back (history[(size_t) ((end - i + historySize) % historySize)].load (std::memory_order_relaxed));

    return out;
}
} // namespace phazite::engine
