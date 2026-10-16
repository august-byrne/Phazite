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

    ~Impl() { detach(); }

    void refresh()
    {
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

LevelSource makeLevelSource (std::function<te::LevelMeasurer*()> findMeasurer)
{
    auto impl = std::make_unique<LevelSource::Impl>();
    impl->find = std::move (findMeasurer);
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
    return impl != nullptr && impl->measurer.get() != nullptr;
}

int LevelSource::getNumChannels() const
{
    return isMeasuring() ? impl->client.getNumChannelsUsed() : 0;
}

float LevelSource::getAndClearLevel (int channel)
{
    return isMeasuring() ? impl->client.getAndClearAudioLevel (channel).dB : silenceDb;
}
} // namespace phazite
