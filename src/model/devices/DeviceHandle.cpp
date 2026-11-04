// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceHandle.h"
#include "internal/DeviceHandle.h"
#include "internal/Devices.h"
#include "internal/DrumKit.h"
#include "internal/ParameterHandle.h"
#include "internal/Wavetables.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
struct DeviceHandle::Impl
{
    juce::WeakReference<Song> song;
    DeviceID id;
};

namespace
{
    // The song, while it holds the device.
    Song* songOf (const std::shared_ptr<const DeviceHandle::Impl>& impl)
    {
        auto* song = impl != nullptr ? impl->song.get() : nullptr;
        return song != nullptr && findDeviceState (*song, impl->id).isValid() ? song : nullptr;
    }

    // The device as Phazite's engine plays it, while it plays the song: what its editor shows and plays is that one's.
    engine::DeviceRunner* findRunner (const std::shared_ptr<const DeviceHandle::Impl>& impl)
    {
        if (auto* song = songOf (impl))
            if (auto* session = findPlayingSession (*song))
                if (auto* node = session->getPlayer().findDevice ((std::int64_t) impl->id.getRaw()))
                    return &node->getRunner();

        return nullptr;
    }
} // namespace

DeviceHandle makeDeviceHandle (Song& song, DeviceID id)
{
    if (findOwnDevice (findDeviceState (song, id)[schema::deviceType].toString().toStdString()) == nullptr)
        return {};

    auto impl = std::make_shared<DeviceHandle::Impl>();
    impl->song = &song;
    impl->id = id;
    return DeviceHandle (std::move (impl));
}

DeviceHandle::DeviceHandle (std::shared_ptr<const Impl> i)
    : impl (std::move (i))
{
}

bool DeviceHandle::isValid() const
{
    return songOf (impl) != nullptr;
}

const devices::DeviceInfo* DeviceHandle::getInfo() const
{
    auto* song = songOf (impl);
    return song != nullptr ? findOwnDevice (findDeviceState (*song, impl->id)[schema::deviceType].toString().toStdString()) : nullptr;
}

ParameterHandle DeviceHandle::getParameter (std::string_view id, const juce::String& undoName) const
{
    if (auto* song = songOf (impl))
        return makeParameterHandle (*song, { impl->id, juce::String (id.data(), id.size()) }, undoName);

    return {};
}

SidechainChoice DeviceHandle::getSidechainSource() const
{
    auto* song = songOf (impl);
    return song != nullptr ? describeSidechainSource (*song, impl->id) : SidechainChoice();
}

juce::String DeviceHandle::getPadName (int pad) const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getPadName (*song, impl->id, pad) : juce::String();
}

bool DeviceHandle::hasPadSound (int pad) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::hasPadSound (*song, impl->id, pad);
}

bool DeviceHandle::setPadSound (int pad, const juce::File& file) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::setPadSound (*song, impl->id, pad, file);
}

void DeviceHandle::clearPadSound (int pad) const
{
    if (auto* song = songOf (impl))
        phazite::clearPadSound (*song, impl->id, pad);
}

juce::String DeviceHandle::getWavetableName (int osc) const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getWavetableName (*song, impl->id, osc) : juce::String();
}

bool DeviceHandle::hasWavetableFile (int osc) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::hasWavetableFile (*song, impl->id, osc);
}

std::shared_ptr<const devices::Wavetable> DeviceHandle::getWavetable (int osc) const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getWavetable (*song, impl->id, osc) : nullptr;
}

void DeviceHandle::setFactoryWavetable (int osc, int index) const
{
    if (auto* song = songOf (impl))
        phazite::setFactoryWavetable (*song, impl->id, osc, index);
}

bool DeviceHandle::setWavetableFile (int osc, const juce::File& file) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::setWavetableFile (*song, impl->id, osc, file);
}

devices::DeviceScope* DeviceHandle::getScope() const
{
    auto* runner = findRunner (impl);
    return runner != nullptr ? &runner->getScope() : nullptr;
}

devices::Processor* DeviceHandle::getProcessorBase() const
{
    auto* runner = findRunner (impl);
    return runner != nullptr ? &runner->getProcessor() : nullptr;
}
} // namespace phazite
