// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixHandle.h"
#include "internal/MixHandle.h"
#include "internal/Mixer.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
struct MixHandle::Impl
{
    te::SafeSelectable<te::Plugin> plugin;
    std::unique_ptr<Gesture> gesture;
};

MixHandle makeMixHandle (te::Plugin& device)
{
    auto impl = std::make_unique<MixHandle::Impl>();
    impl->plugin = &device;
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
    return impl != nullptr && impl->plugin.get() != nullptr;
}

float MixHandle::getValue() const
{
    auto* device = impl != nullptr ? impl->plugin.get() : nullptr;
    return device != nullptr ? getMix (*device) : 1.0f;
}

void MixHandle::beginGesture()
{
    auto* device = impl != nullptr ? impl->plugin.get() : nullptr;

    if (device == nullptr || impl->gesture != nullptr)
        return;

    impl->gesture = std::make_unique<Gesture> (device->edit, "Change Mix");
}

void MixHandle::setValue (float mix)
{
    auto* device = impl != nullptr ? impl->plugin.get() : nullptr;

    if (device == nullptr)
        return;

    if (impl->gesture != nullptr)
    {
        setMix (*device, mix);
        return;
    }

    const UndoStep step (device->edit, "Change Mix");
    setMix (*device, mix);
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
