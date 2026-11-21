// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/MidiTap.h"

namespace phazite
{
MidiTap::MidiTap() = default;

MidiTap::~MidiTap()
{
    cancelPendingUpdate();
}

bool MidiTap::take (const juce::MidiMessage& message) noexcept
{
    if (! message.isController())
        return false;

    const auto channel = message.getChannel();
    const auto number = message.getControllerNumber();

    if (channel < 1 || channel > numChannels || number < 0 || number >= numLinkable)
        return false;

    if (! learning.load (std::memory_order_acquire) && ! isDiverted (channel, number))
        return false;

    if (! queue.push ({ (std::uint8_t) channel, (std::uint8_t) number, (std::uint8_t) message.getControllerValue() }))
        dropped.fetch_add (1, std::memory_order_relaxed);

    // A MIDI input's thread is not the audio thread: posting to the message thread is allowed here, and is posted once until
    // it has run (juce::AsyncUpdater).
    triggerAsyncUpdate();
    return true;
}

void MidiTap::setDiverted (const std::vector<std::pair<int, int>>& controllers)
{
    std::array<std::uint64_t, (size_t) (numChannels * wordsPerChannel)> words {};

    for (const auto& [channel, number] : controllers)
        if (channel >= 1 && channel <= numChannels && number >= 0 && number < numLinkable)
            words[(size_t) ((channel - 1) * wordsPerChannel + number / 64)] |= (std::uint64_t) 1 << (number % 64);

    for (size_t i = 0; i < words.size(); ++i)
        diverted[i].store (words[i], std::memory_order_release);
}

void MidiTap::setLearning (bool on) noexcept
{
    learning.store (on, std::memory_order_release);
}

bool MidiTap::isDiverted (int channel, int number) const noexcept
{
    if (channel < 1 || channel > numChannels || number < 0 || number >= numLinkable)
        return false;

    const auto word = diverted[(size_t) ((channel - 1) * wordsPerChannel + number / 64)].load (std::memory_order_acquire);
    return (word & ((std::uint64_t) 1 << (number % 64))) != 0;
}

bool MidiTap::pop (Value& value) noexcept
{
    return queue.pop (value);
}

void MidiTap::clear() noexcept
{
    for (Value value; queue.pop (value);)
    {
    }
}

void MidiTap::handleAsyncUpdate()
{
    if (onArrived != nullptr)
        onArrived();
}
} // namespace phazite
