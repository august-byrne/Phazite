// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BlockChannel.h"

#include <type_traits>

namespace phazite::sandbox
{
namespace
{
    constexpr std::size_t roundUp (std::size_t size) noexcept
    {
        return (size + 63) & ~(std::size_t) 63;
    }

    // Where each part of a channel's memory starts.
    struct Places
    {
        std::size_t audio = 0, events = 0, changes = 0, size = 0;
    };

    Places getPlaces (std::size_t headerSize, const BlockChannel::Limits& limits)
    {
        Places places;
        places.audio = roundUp (headerSize);
        const auto numChannels = (std::size_t) (limits.maxChannels + limits.maxSidechainChannels);
        places.events = roundUp (places.audio + numChannels * (std::size_t) limits.maxSamples * sizeof (float));
        places.changes = roundUp (places.events + (std::size_t) limits.maxEvents * sizeof (MidiEvent));
        places.size = roundUp (places.changes + (std::size_t) limits.maxChanges * sizeof (ParameterChange));
        return places;
    }
} // namespace

std::unique_ptr<BlockChannel> BlockChannel::create (const std::string& name, const Limits& limits)
{
    return make (name, limits, true);
}

std::unique_ptr<BlockChannel> BlockChannel::open (const std::string& name, const Limits& limits)
{
    return make (name, limits, false);
}

std::unique_ptr<BlockChannel> BlockChannel::make (const std::string& name, const Limits& limits, bool create)
{
    static_assert (std::is_trivially_copyable_v<BlockInfo> && std::is_trivially_copyable_v<Limits>);

    if (limits.maxSamples <= 0 || limits.maxChannels <= 0)
        return nullptr;

    const auto places = getPlaces (sizeof (Header), limits);
    std::unique_ptr<BlockChannel> channel (new BlockChannel());
    channel->memory = create ? SharedMemory::create (name, places.size) : SharedMemory::open (name, places.size);

    if (channel->memory == nullptr)
        return nullptr;

    auto* base = static_cast<char*> (channel->memory->getData());

    // The maker sets the header up in the memory, made zeroed; the other side finds it so.
    if (create)
    {
        channel->header = new (base) Header();
        channel->header->limits = limits;
    }
    else
    {
        channel->header = reinterpret_cast<Header*> (base);
    }

    channel->limits = limits;
    channel->audio = reinterpret_cast<float*> (base + places.audio);
    channel->events = reinterpret_cast<MidiEvent*> (base + places.events);
    channel->changes = reinterpret_cast<ParameterChange*> (base + places.changes);

    auto& request = channel->header->request.state;
    auto& reply = channel->header->reply.state;
    channel->requestBell = create ? Bell::create (name + "-q", request) : Bell::open (name + "-q", request);
    channel->replyBell = create ? Bell::create (name + "-a", reply) : Bell::open (name + "-a", reply);

    if (channel->requestBell == nullptr || channel->replyBell == nullptr)
        return nullptr;

    // Where each side starts counting: where the counters are now (the helper may open it after blocks were refused).
    channel->handed = channel->replyBell->get();
    channel->taken = channel->requestBell->get();
    return channel;
}

BlockChannel::Played BlockChannel::play (std::int64_t spinNanoseconds, std::int64_t timeoutNanoseconds) noexcept
{
    if (! handOver())
        return Played::busy;

    return waitForAnswer (spinNanoseconds, timeoutNanoseconds);
}

bool BlockChannel::handOver() noexcept
{
    if (isBusy())
        return false;

    requestBell->ring (++handed);
    return true;
}

BlockChannel::Played BlockChannel::waitForAnswer (std::int64_t spinNanoseconds, std::int64_t timeoutNanoseconds) noexcept
{
    return replyBell->waitWhile (handed - 1, spinNanoseconds, timeoutNanoseconds) ? Played::done : Played::late;
}

bool BlockChannel::waitForBlock (std::int64_t timeoutNanoseconds) noexcept
{
    if (! requestBell->waitWhile (taken, 0, timeoutNanoseconds))
        return false;

    taken = requestBell->get();
    return true;
}

void BlockChannel::finish() noexcept
{
    replyBell->ring (taken);
}
} // namespace phazite::sandbox
