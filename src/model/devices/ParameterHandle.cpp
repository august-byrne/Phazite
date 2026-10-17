// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterHandle.h"
#include "internal/DevicePlugin.h"
#include "internal/ParameterHandle.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
struct ParameterHandle::Impl
{
    te::SafeSelectable<te::Plugin> plugin;
    juce::String parameterID, undoName;
    std::unique_ptr<Gesture> gesture; // the gesture it began,
    bool joined = false;              // or whether it joined another change's

    te::AutomatableParameter::Ptr find() const
    {
        if (auto* p = plugin.get())
            return p->getAutomatableParameterByID (parameterID);

        return {};
    }
};

namespace
{
    // Tracktion stores a parameter's value only once it first changes, so undoing that first change would remove the
    // value again, and phazite::undo() would put it back as a step of its own, which would cost the redo history. So
    // the value is stored first, without an undo step: with the value unchanged, setParameter() only writes it, past
    // the undo manager.
    void storeValueWithoutUndo (te::AutomatableParameter& parameter)
    {
        parameter.setParameter (parameter.getCurrentValue(), juce::sendNotification);
    }
} // namespace

ParameterHandle makeParameterHandle (te::Plugin* plugin, const juce::String& parameterID, const juce::String& undoName)
{
    if (plugin == nullptr)
        return {};

    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->plugin = plugin;
    impl->parameterID = parameterID;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

ParameterHandle::ParameterHandle() = default;

ParameterHandle::ParameterHandle (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

ParameterHandle::ParameterHandle (ParameterHandle&& other) noexcept = default;

ParameterHandle& ParameterHandle::operator= (ParameterHandle&& other) noexcept
{
    if (this != &other)
    {
        endGesture();
        impl = std::move (other.impl);
    }

    return *this;
}

ParameterHandle::~ParameterHandle()
{
    endGesture();
}

bool ParameterHandle::isValid() const
{
    return impl != nullptr && impl->find() != nullptr;
}

bool ParameterHandle::refersToSameParameter (const ParameterHandle& other) const
{
    if (impl == nullptr || other.impl == nullptr)
        return impl == other.impl;

    return impl->plugin.get() == other.impl->plugin.get() && impl->parameterID == other.impl->parameterID;
}

ParameterRef ParameterHandle::getRef() const
{
    if (auto* plugin = impl != nullptr ? impl->plugin.get() : nullptr)
        return { getDeviceID (*plugin), impl->parameterID };

    return {};
}

juce::String ParameterHandle::getName() const
{
    if (const auto parameter = impl != nullptr ? impl->find() : nullptr)
        return parameter->getParameterName();

    return {};
}

juce::NormalisableRange<float> ParameterHandle::getRange() const
{
    if (const auto parameter = impl != nullptr ? impl->find() : nullptr)
        return parameter->valueRange;

    return {};
}

std::optional<float> ParameterHandle::getDefaultValue() const
{
    if (const auto parameter = impl != nullptr ? impl->find() : nullptr)
        return parameter->getDefaultValue();

    return {};
}

float ParameterHandle::getValue() const
{
    if (const auto parameter = impl != nullptr ? impl->find() : nullptr)
        return parameter->getCurrentValue();

    return 0.0f;
}

juce::String ParameterHandle::getText (float value) const
{
    if (const auto parameter = impl != nullptr ? impl->find() : nullptr)
        return parameter->valueToString (value);

    return {};
}

std::optional<devices::ParameterInfo> ParameterHandle::getInfo() const
{
    if (auto* device = impl != nullptr ? dynamic_cast<DevicePlugin*> (impl->plugin.get()) : nullptr)
    {
        const auto& info = device->getDeviceInfo();
        const auto id = impl->parameterID.toStdString();

        if (const auto index = devices::findParameter (info, id); index >= 0)
            return info.parameters[(size_t) index];
    }

    return {};
}

void ParameterHandle::beginGesture()
{
    const auto parameter = impl != nullptr ? impl->find() : nullptr;

    if (parameter == nullptr || isInGesture())
        return;

    storeValueWithoutUndo (*parameter);

    // Within another change's gesture (a drag that moves two parameters), it joins that gesture's step.
    if (Gesture::isInProgress (parameter->getEdit()))
        impl->joined = true;
    else
        impl->gesture = std::make_unique<Gesture> (
            parameter->getEdit(), impl->undoName.isNotEmpty() ? impl->undoName : "Change " + parameter->getParameterName());

    parameter->parameterChangeGestureBegin(); // for automation recording
}

void ParameterHandle::setValue (float value)
{
    const auto parameter = impl != nullptr ? impl->find() : nullptr;

    if (parameter == nullptr)
        return;

    if (isInGesture())
    {
        parameter->setParameter (value, juce::sendNotification);
        return;
    }

    storeValueWithoutUndo (*parameter);
    const UndoStep step (parameter->getEdit(), impl->undoName.isNotEmpty() ? impl->undoName : "Change " + parameter->getParameterName());
    parameter->setParameter (value, juce::sendNotification);
}

void ParameterHandle::endGesture()
{
    if (! isInGesture())
        return;

    if (const auto parameter = impl->find())
        parameter->parameterChangeGestureEnd();

    if (impl->gesture != nullptr)
    {
        impl->gesture->commit();
        impl->gesture.reset();
    }

    impl->joined = false;
}

bool ParameterHandle::isInGesture() const
{
    return impl != nullptr && (impl->gesture != nullptr || impl->joined);
}
} // namespace phazite
