// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LevelSource.h"
#include "internal/LevelSource.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite
{
struct LevelSource::Impl
{
    // What finds the meter, the one it found last, and where this reads it from, which starts again with another meter.
    std::function<engine::LevelMeter*()> find;
    engine::LevelMeter* meter = nullptr;
    engine::LevelMeter::Reader reader;

    // The meter now, found again each time: what plays the song makes its meters anew (another plan, another device), and
    // one that has gone is never read.
    engine::LevelMeter* refresh()
    {
        auto* now = find != nullptr ? find() : nullptr;

        if (now != meter)
            reader = {};

        meter = now;
        return meter;
    }
};

LevelSource makeLevelSource (std::function<engine::LevelMeter*()> find)
{
    auto impl = std::make_unique<LevelSource::Impl>();
    impl->find = std::move (find);
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
    return impl != nullptr && impl->refresh() != nullptr;
}

int LevelSource::getNumChannels() const
{
    auto* meter = impl != nullptr ? impl->refresh() : nullptr;
    return meter != nullptr ? meter->getNumChannels() : 0;
}

float LevelSource::getAndClearLevel (int channel)
{
    auto* meter = impl != nullptr ? impl->refresh() : nullptr;
    return meter != nullptr ? juce::Decibels::gainToDecibels (meter->getPeakSince (impl->reader, channel), silenceDb) : silenceDb;
}
} // namespace phazite
