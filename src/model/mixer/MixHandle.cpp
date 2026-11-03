// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixHandle.h"
#include "internal/MixHandle.h"
#include "internal/Mixer.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
struct MixHandle::Impl
{
    te::SafeSelectable<te::Edit> edit;
    DeviceID device;
    std::unique_ptr<Gesture> gesture;

    // The song's Edit while it holds the device.
    te::Edit* findEdit() const
    {
        auto* in = edit.get();
        return in != nullptr && findDeviceState (*in, device).isValid() ? in : nullptr;
    }
};

MixHandle makeMixHandle (te::Plugin& device)
{
    auto impl = std::make_unique<MixHandle::Impl>();
    impl->edit = &device.edit;
    impl->device = getDeviceID (device);
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
    return impl != nullptr && impl->findEdit() != nullptr;
}

float MixHandle::getValue() const
{
    auto* edit = impl != nullptr ? impl->findEdit() : nullptr;
    return edit != nullptr ? getMix (getSong (*edit), impl->device) : 1.0f;
}

void MixHandle::beginGesture()
{
    auto* edit = impl != nullptr ? impl->findEdit() : nullptr;

    if (edit == nullptr || impl->gesture != nullptr)
        return;

    impl->gesture = std::make_unique<Gesture> (*edit, "Change Mix");
}

void MixHandle::setValue (float mix)
{
    auto* edit = impl != nullptr ? impl->findEdit() : nullptr;

    if (edit == nullptr)
        return;

    if (impl->gesture != nullptr)
    {
        setMix (getSong (*edit), impl->device, mix);
        return;
    }

    const UndoStep step (*edit, "Change Mix");
    setMix (getSong (*edit), impl->device, mix);
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
