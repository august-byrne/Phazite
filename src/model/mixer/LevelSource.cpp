// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LevelSource.h"
#include "internal/LevelSource.h"

namespace phazite
{
struct LevelSource::Impl
{
    std::function<te::LevelMeasurer*()> find;
    juce::WeakReference<te::LevelMeasurer> measurer;
    te::LevelMeasurer::Client client;

    // Phazite's own engine's meter, while it plays the channel, and where this reads it from.
    std::function<engine::LevelMeter*()> findOwn;
    engine::LevelMeter* own = nullptr;
    engine::LevelMeter::Reader reader;

    ~Impl() { detach(); }

    void refresh()
    {
        if (auto* ownNow = findOwn != nullptr ? findOwn() : nullptr)
        {
            detach();

            if (ownNow != own)
                reader = {};

            own = ownNow;
            return;
        }

        own = nullptr;
        auto* now = find != nullptr ? find() : nullptr;

        if (now == measurer.get())
            return;

        detach();
        measurer = now;

        if (now != nullptr)
            now->addClient (client);
    }

    void detach()
    {
        if (auto* old = measurer.get())
            old->removeClient (client);

        measurer = nullptr;
        client.reset();
    }
};

LevelSource makeLevelSource (std::function<te::LevelMeasurer*()> findMeasurer, std::function<engine::LevelMeter*()> findOwn)
{
    auto impl = std::make_unique<LevelSource::Impl>();
    impl->find = std::move (findMeasurer);
    impl->findOwn = std::move (findOwn);
    impl->refresh();
    return LevelSource (std::move (impl));
}

LevelSource::LevelSource() = default;

LevelSource::LevelSource (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

LevelSource::LevelSource (LevelSource&&) noexcept = default;
LevelSource& LevelSource::operator= (LevelSource&&) noexcept = default;
LevelSource::~LevelSource() = default;

void LevelSource::refresh()
{
    if (impl != nullptr)
        impl->refresh();
}

bool LevelSource::isMeasuring() const
{
    return impl != nullptr && (impl->own != nullptr || impl->measurer.get() != nullptr);
}

int LevelSource::getNumChannels() const
{
    if (impl != nullptr && impl->own != nullptr)
        return impl->own->getNumChannels();

    return isMeasuring() ? impl->client.getNumChannelsUsed() : 0;
}

float LevelSource::getAndClearLevel (int channel)
{
    if (impl != nullptr && impl->own != nullptr)
        return juce::Decibels::gainToDecibels (impl->own->getPeakSince (impl->reader, channel), silenceDb);

    return isMeasuring() ? impl->client.getAndClearAudioLevel (channel).dB : silenceDb;
}
} // namespace phazite
