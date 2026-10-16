// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixHandle.h"
#include "Mixer.h"
#include "internal/MixHandle.h"
#include "model/commands/Undo.h"

namespace phazite
{
struct MixHandle::Impl
{
    te::SafeSelectable<te::Plugin> plugin;
    std::unique_ptr<Gesture> gesture;
};

namespace
{
    // As ParameterHandle does, the levels are stored first, without an undo step, so that undoing the first change plays
    // the mix there was.
    void storeMixWithoutUndo (te::Plugin& device)
    {
        for (auto* parameter : getMixParameters (device))
            parameter->setParameter (parameter->getCurrentValue(), juce::sendNotification);
    }
} // namespace

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

    storeMixWithoutUndo (*device);
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

    storeMixWithoutUndo (*device);
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
