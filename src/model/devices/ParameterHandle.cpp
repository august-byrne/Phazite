// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterHandle.h"
#include "DevicePlugin.h"
#include "model/commands/Undo.h"

#include <utility>

namespace phazite
{
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

ParameterHandle::ParameterHandle() = default;

ParameterHandle::ParameterHandle (te::Plugin* p, const juce::String& id, const juce::String& name)
    : plugin (p),
      parameterID (id),
      undoName (name)
{
}

ParameterHandle::ParameterHandle (ParameterHandle&& other) noexcept
    : plugin (std::move (other.plugin)),
      parameterID (std::move (other.parameterID)),
      undoName (std::move (other.undoName)),
      gesture (std::move (other.gesture)),
      joined (std::exchange (other.joined, false))
{
}

ParameterHandle& ParameterHandle::operator= (ParameterHandle&& other) noexcept
{
    if (this != &other)
    {
        endGesture();
        plugin = std::move (other.plugin);
        parameterID = std::move (other.parameterID);
        undoName = std::move (other.undoName);
        gesture = std::move (other.gesture);
        joined = std::exchange (other.joined, false);
    }

    return *this;
}

ParameterHandle::~ParameterHandle()
{
    endGesture();
}

te::AutomatableParameter::Ptr ParameterHandle::find() const
{
    if (auto* p = plugin.get())
        return p->getAutomatableParameterByID (parameterID);

    return {};
}

bool ParameterHandle::isValid() const
{
    return find() != nullptr;
}

bool ParameterHandle::refersTo (const te::Plugin* other, const juce::String& otherID) const
{
    return plugin.get() == other && parameterID == otherID;
}

juce::String ParameterHandle::getName() const
{
    if (const auto parameter = find())
        return parameter->getParameterName();

    return {};
}

juce::NormalisableRange<float> ParameterHandle::getRange() const
{
    if (const auto parameter = find())
        return parameter->valueRange;

    return {};
}

std::optional<float> ParameterHandle::getDefaultValue() const
{
    if (const auto parameter = find())
        return parameter->getDefaultValue();

    return {};
}

float ParameterHandle::getValue() const
{
    if (const auto parameter = find())
        return parameter->getCurrentValue();

    return 0.0f;
}

juce::String ParameterHandle::getText (float value) const
{
    if (const auto parameter = find())
        return parameter->valueToString (value);

    return {};
}

std::optional<devices::ParameterInfo> ParameterHandle::getInfo() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (plugin.get()))
    {
        const auto& info = device->getDeviceInfo();
        const auto id = parameterID.toStdString();

        if (const auto index = devices::findParameter (info, id); index >= 0)
            return info.parameters[(size_t) index];
    }

    return {};
}

void ParameterHandle::beginGesture()
{
    const auto parameter = find();

    if (parameter == nullptr || isInGesture())
        return;

    storeValueWithoutUndo (*parameter);

    // Within another change's gesture (a drag that moves two parameters), it joins that gesture's step.
    if (Gesture::isInProgress (parameter->getEdit()))
        joined = true;
    else
        gesture =
            std::make_unique<Gesture> (parameter->getEdit(), undoName.isNotEmpty() ? undoName : "Change " + parameter->getParameterName());

    parameter->parameterChangeGestureBegin(); // for automation recording
}

void ParameterHandle::setValue (float value)
{
    const auto parameter = find();

    if (parameter == nullptr)
        return;

    if (isInGesture())
    {
        parameter->setParameter (value, juce::sendNotification);
        return;
    }

    storeValueWithoutUndo (*parameter);
    const UndoStep step (parameter->getEdit(), undoName.isNotEmpty() ? undoName : "Change " + parameter->getParameterName());
    parameter->setParameter (value, juce::sendNotification);
}

void ParameterHandle::endGesture()
{
    if (! isInGesture())
        return;

    if (const auto parameter = find())
        parameter->parameterChangeGestureEnd();

    if (gesture != nullptr)
    {
        gesture->commit();
        gesture.reset();
    }

    joined = false;
}

bool ParameterHandle::isInGesture() const
{
    return gesture != nullptr || joined;
}
} // namespace phazite
