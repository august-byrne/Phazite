// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterHandle.h"
#include "internal/DevicePlugin.h"
#include "internal/ParameterHandle.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
struct ParameterHandle::Impl
{
    te::SafeSelectable<te::Plugin> plugin;     // a device's parameter,
    te::SafeSelectable<te::Modifier> modifier; // a modulator's,
    te::SafeSelectable<te::AudioTrack> macros; // one of a track's macros,
    te::SafeSelectable<te::RackType> rack;     // or one of a container's
    juce::String parameterID, undoName;
    std::unique_ptr<Gesture> gesture; // the gesture it began,
    bool joined = false;              // or whether it joined another change's

    // A device that may not be there yet, on a track, found each time, which the first change makes
    // (makeCreatingHandle()), in a step of createName; and the range its parameter shows until then.
    te::SafeSelectable<te::Track> owner;
    std::function<te::Plugin*(te::Track&)> locate, create;
    juce::String identity; // which device it is, for refersToSameParameter() while it isn't there
    juce::String createName;
    juce::NormalisableRange<float> rangeUntilMade;

    te::Plugin* locateDevice() const
    {
        auto* track = owner.get();
        return track != nullptr && locate != nullptr ? locate (*track) : nullptr;
    }

    te::AutomatableParameter::Ptr find() const
    {
        if (locate != nullptr)
        {
            if (auto* p = locateDevice())
                return p->getAutomatableParameterByID (parameterID);

            return {};
        }

        if (auto* p = plugin.get())
            return p->getAutomatableParameterByID (parameterID);

        if (auto* m = modifier.get())
            return m->getAutomatableParameterByID (parameterID);

        if (auto* t = macros.get())
            if (auto* list = t->getMacroParameterList())
                return list->getAutomatableParameterByID (parameterID);

        if (auto* r = rack.get())
            if (auto* list = r->getMacroParameterList())
                return list->getAutomatableParameterByID (parameterID);

        return {};
    }

    const void* getOwner() const
    {
        if (locate != nullptr)
            return locateDevice();

        if (auto* p = plugin.get())
            return p;

        if (auto* m = modifier.get())
            return m;

        if (auto* t = macros.get())
            return t;

        return rack.get();
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

ParameterHandle makeParameterHandle (te::Modifier& modifier, const juce::String& parameterID, const juce::String& undoName)
{
    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->modifier = &modifier;
    impl->parameterID = parameterID;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

ParameterHandle makeMacroHandle (te::AudioTrack& track, const te::MacroParameter& macro, const juce::String& undoName)
{
    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->macros = &track;
    impl->parameterID = macro.paramID;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

ParameterHandle makeMacroHandle (te::RackType& container, const te::MacroParameter& macro, const juce::String& undoName)
{
    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->rack = &container;
    impl->parameterID = macro.paramID;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

ParameterHandle makeCreatingHandle (te::Track& owner, std::function<te::Plugin*(te::Track&)> locate,
                                    std::function<te::Plugin*(te::Track&)> create, const juce::String& identity,
                                    const juce::String& parameterID, juce::NormalisableRange<float> range, const juce::String& createName,
                                    const juce::String& undoName)
{
    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->owner = &owner;
    impl->locate = std::move (locate);
    impl->create = std::move (create);
    impl->identity = identity;
    impl->parameterID = parameterID;
    impl->rangeUntilMade = range;
    impl->createName = createName;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

te::AutomatableParameter::Ptr findParameter (const ParameterHandle& handle)
{
    return handle.getImpl() != nullptr ? handle.getImpl()->find() : nullptr;
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
    return exists() || (impl != nullptr && impl->create != nullptr);
}

bool ParameterHandle::exists() const
{
    return impl != nullptr && impl->find() != nullptr;
}

bool ParameterHandle::refersToSameParameter (const ParameterHandle& other) const
{
    if (impl == nullptr || other.impl == nullptr)
        return impl == other.impl;

    if (impl->identity.isNotEmpty() || other.impl->identity.isNotEmpty())
        return impl->identity == other.impl->identity && impl->parameterID == other.impl->parameterID;

    return impl->getOwner() == other.impl->getOwner() && impl->parameterID == other.impl->parameterID;
}

ParameterRef ParameterHandle::getRef() const
{
    if (impl == nullptr)
        return {};

    if (auto* plugin = impl->locate != nullptr ? impl->locateDevice() : impl->plugin.get())
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

    return impl != nullptr && impl->create != nullptr ? impl->rangeUntilMade : juce::NormalisableRange<float>();
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

    return impl != nullptr && impl->create != nullptr ? impl->rangeUntilMade.start : 0.0f; // the start of the range until made
}

float ParameterHandle::getBaseValue() const
{
    if (const auto parameter = impl != nullptr ? impl->find() : nullptr)
        return parameter->getCurrentBaseValue();

    return impl != nullptr && impl->create != nullptr ? impl->rangeUntilMade.start : 0.0f; // the start of the range until made
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
    if (impl == nullptr || isInGesture())
        return;

    const auto parameter = impl->find();

    // A device the first value makes: the gesture's step begins now, and a press without a drag makes nothing.
    if (parameter == nullptr)
    {
        auto* track = impl->owner.get();

        if (impl->create == nullptr || track == nullptr)
            return;

        if (Gesture::isInProgress (track->edit))
            impl->joined = true;
        else
            impl->gesture = std::make_unique<Gesture> (track->edit, impl->createName);

        return;
    }

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
    if (impl == nullptr)
        return;

    if (impl->find() == nullptr)
    {
        makeAndSet (value);
        return;
    }

    const auto parameter = impl->find();

    if (isInGesture())
    {
        parameter->setParameter (value, juce::sendNotification);
        return;
    }

    storeValueWithoutUndo (*parameter);
    const UndoStep step (parameter->getEdit(), impl->undoName.isNotEmpty() ? impl->undoName : "Change " + parameter->getParameterName());
    parameter->setParameter (value, juce::sendNotification);
}

void ParameterHandle::makeAndSet (float value)
{
    auto* track = impl->owner.get();

    if (impl->create == nullptr || track == nullptr)
        return;

    // In the gesture's step, or a step of its own for a value set outside a gesture.
    std::optional<UndoStep> step;

    if (! isInGesture())
        step.emplace (track->edit, impl->createName);

    auto* made = impl->create (*track);
    const auto parameter = made != nullptr ? made->getAutomatableParameterByID (impl->parameterID) : nullptr;

    if (parameter == nullptr)
        return;

    if (isInGesture())
        parameter->parameterChangeGestureBegin();

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
