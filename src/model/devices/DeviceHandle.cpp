// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceHandle.h"
#include "internal/DeviceHandle.h"
#include "internal/Devices.h"
#include "internal/DrumKit.h"
#include "internal/ParameterHandle.h"
#include "internal/SamplerSound.h"
#include "internal/Swap.h"
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

juce::String DeviceHandle::getPadName (int pad, int layer) const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getPadName (*song, impl->id, pad, layer) : juce::String();
}

bool DeviceHandle::hasPadSound (int pad, int layer) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::hasPadSound (*song, impl->id, pad, layer);
}

bool DeviceHandle::isPadEmpty (int pad) const
{
    auto* song = songOf (impl);
    return song == nullptr || phazite::isPadEmpty (*song, impl->id, pad);
}

bool DeviceHandle::setPadSound (int pad, const juce::File& file, int layer) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::setPadSound (*song, impl->id, pad, file, layer);
}

void DeviceHandle::clearPadSound (int pad, int layer) const
{
    if (auto* song = songOf (impl))
        phazite::clearPadSound (*song, impl->id, pad, layer);
}

TrackID DeviceHandle::getPadTrack (int pad) const
{
    auto* song = songOf (impl);
    return song != nullptr ? findPadTrack (*song, impl->id, pad) : TrackID();
}

void DeviceHandle::setPadOnOwnTrack (int pad, bool onOwnTrack) const
{
    if (auto* song = songOf (impl))
    {
        if (onOwnTrack)
            givePadItsOwnTrack (*song, impl->id, pad);
        else
            bringPadBack (*song, impl->id, pad);
    }
}

std::optional<int> DeviceHandle::getSwap() const
{
    auto* song = songOf (impl);
    return song != nullptr ? findOpenSwap (*song, impl->id) : std::nullopt;
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

juce::String DeviceHandle::getSampleName() const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getSampleName (*song, impl->id) : juce::String();
}

bool DeviceHandle::hasSampleFile() const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::hasSampleFile (*song, impl->id);
}

int DeviceHandle::getFactorySample() const
{
    auto* song = songOf (impl);
    return song != nullptr ? getFactorySampleIndex (*song, impl->id) : -1;
}

bool DeviceHandle::setSampleFile (const juce::File& file) const
{
    auto* song = songOf (impl);
    return song != nullptr && phazite::setSampleFile (*song, impl->id, file);
}

void DeviceHandle::setFactorySample (int index) const
{
    if (auto* song = songOf (impl))
        phazite::setFactorySample (*song, impl->id, index);
}

void DeviceHandle::findRootKey() const
{
    if (auto* song = songOf (impl))
        phazite::findRootKey (*song, impl->id);
}

std::shared_ptr<const devices::Sampler::Sound> DeviceHandle::getSampleSound() const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getSampleSound (*song, impl->id) : nullptr;
}

devices::Sampler::ManualSlices DeviceHandle::getManualSlices() const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getManualSlices (*song, impl->id) : devices::Sampler::ManualSlices();
}

std::vector<float> DeviceHandle::getSlicePositions() const
{
    auto* song = songOf (impl);
    return song != nullptr ? phazite::getSlicePositions (*song, impl->id) : std::vector<float>();
}

void DeviceHandle::moveSlice (int slice, float position) const
{
    if (auto* song = songOf (impl))
        phazite::moveSlice (*song, impl->id, slice, position);
}

void DeviceHandle::addSlice (float position) const
{
    if (auto* song = songOf (impl))
        phazite::addSlice (*song, impl->id, position);
}

void DeviceHandle::removeSlice (int slice) const
{
    if (auto* song = songOf (impl))
        phazite::removeSlice (*song, impl->id, slice);
}

void DeviceHandle::writeSliceNotes() const
{
    if (auto* song = songOf (impl))
        phazite::writeSliceNotes (*song, impl->id, nullptr);
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
