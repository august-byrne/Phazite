// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixHandle.h"
#include "internal/MixHandle.h"
#include "internal/Mixer.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
struct MixHandle::Impl
{
    juce::WeakReference<Song> song;
    DeviceID device;
    std::unique_ptr<Gesture> gesture;

    // The song while it lives and holds the device.
    Song* findSong() const
    {
        auto* in = song.get();
        return in != nullptr && findDeviceState (*in, device).isValid() ? in : nullptr;
    }
};

MixHandle makeMixHandle (Song& song, DeviceID device)
{
    auto impl = std::make_unique<MixHandle::Impl>();
    impl->song = &song;
    impl->device = device;
    return MixHandle (std::move (impl));
}

MixHandle::MixHandle() = default;

MixHandle::MixHandle (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

MixHandle::MixHandle (MixHandle&&) noexcept = default;

MixHandle& MixHandle::operator= (MixHandle&& other) noexcept
{
    if (this != &other)
    {
        endGesture();
        impl = std::move (other.impl);
    }

    return *this;
}

MixHandle::~MixHandle()
{
    endGesture();
}

bool MixHandle::isValid() const
{
    return impl != nullptr && impl->findSong() != nullptr;
}

float MixHandle::getValue() const
{
    auto* song = impl != nullptr ? impl->findSong() : nullptr;
    return song != nullptr ? getMix (*song, impl->device) : 1.0f;
}

float MixHandle::getDefaultValue() const
{
    auto* song = impl != nullptr ? impl->findSong() : nullptr;
    return song != nullptr ? getDefaultMix (*song, impl->device) : 1.0f;
}

void MixHandle::beginGesture()
{
    auto* song = impl != nullptr ? impl->findSong() : nullptr;

    if (song == nullptr || impl->gesture != nullptr)
        return;

    impl->gesture = std::make_unique<Gesture> (*song, "Change Mix");
}

void MixHandle::setValue (float mix)
{
    auto* song = impl != nullptr ? impl->findSong() : nullptr;

    if (song == nullptr)
        return;

    if (impl->gesture != nullptr)
    {
        setMix (*song, impl->device, mix);
        return;
    }

    const UndoStep step (*song, "Change Mix");
    setMix (*song, impl->device, mix);
}

void MixHandle::endGesture()
{
    if (impl != nullptr && impl->gesture != nullptr)
    {
        impl->gesture->commit();
        impl->gesture.reset();
    }
}
} // namespace phazite
