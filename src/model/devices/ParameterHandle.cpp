// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterHandle.h"
#include "internal/ParameterHandle.h"
#include "internal/Parameters.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
struct ParameterHandle::Impl
{
    juce::WeakReference<Song> song; // the context can outlive it
    ParameterRef ref;
    bool automatable = true; // a device's, a fader's or a send's, which automation names
    juce::String undoName;
    std::unique_ptr<Gesture> gesture; // the gesture it began,
    bool joined = false;              // or whether it joined another change's

    // A device that may not be there yet, found each time, which the first change makes (makeCreatingHandle()), in a step
    // of createName; and the range its parameter shows until then.
    std::function<DeviceID (const Song&)> locate;
    std::function<DeviceID (Song&)> create;
    juce::String identity; // which device it is, for refersToSameParameter() while it isn't there
    juce::String createName;
    juce::NormalisableRange<float> rangeUntilMade;

    mutable std::optional<FoundParameter> cache; // as find() found it last

    // Its parameter while the song has it, or nullptr: found again only once its element has left the song or names another
    // (controls read it at each of the display ticker's ticks: docs/performance-audit.md, U5).
    const FoundParameter* find() const
    {
        auto* in = song.get();

        if (in == nullptr)
            return nullptr;

        auto wanted = ref;

        if (locate != nullptr)
            wanted.device = locate (*in);

        if (! cache.has_value() || cache->ref != wanted || ! isStillThere (*in, *cache))
            cache = locateParameter (*in, wanted);

        return cache.has_value() ? &*cache : nullptr;
    }

    ParameterRef findRef() const
    {
        const auto* parameter = find();
        return parameter != nullptr ? parameter->ref : ParameterRef();
    }

    std::optional<ParameterDescription> describe() const
    {
        const auto* parameter = find();
        return parameter != nullptr ? std::optional (describeParameter (*song.get(), *parameter)) : std::nullopt;
    }

    juce::String getUndoName (const ParameterRef& found) const
    {
        if (undoName.isNotEmpty())
            return undoName;

        const auto description = describeParameter (*song.get(), found);
        return "Change " + (description.has_value() ? description->name : juce::String());
    }

    // Tracktion's parameter, which records a gesture (and Phazite's automation writer, through it), while it mirrors the
    // song's (docs/tracktion-migration.md, T6 step 4 moves the writer to references).
    te::AutomatableParameter::Ptr findTracktionParameter (const ParameterRef& found) const
    {
        auto* edit = song.get() != nullptr ? findEdit (*song.get()) : nullptr;
        return edit != nullptr && found.isValid() ? phazite::findParameter (*edit, found) : nullptr;
    }

    void beginRecording (const ParameterRef& found) const
    {
        if (const auto parameter = findTracktionParameter (found))
        {
            parameter->parameterChangeGestureBegin();

            if (auto* session = findPlayingSession (*song.get()))
                session->getWriter().gestureBegan (*parameter);
        }
    }
};

ParameterHandle makeParameterHandle (Song& song, const ParameterRef& ref, const juce::String& undoName, bool automatable)
{
    if (! ref.isValid())
        return {};

    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->song = &song;
    impl->ref = ref;
    impl->automatable = automatable;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

ParameterHandle makeCreatingHandle (Song& song, std::function<DeviceID (const Song&)> locate, std::function<DeviceID (Song&)> create,
                                    const juce::String& identity, const juce::String& parameterID, juce::NormalisableRange<float> range,
                                    const juce::String& createName, const juce::String& undoName)
{
    auto impl = std::make_unique<ParameterHandle::Impl>();
    impl->song = &song;
    impl->ref = { {}, parameterID };
    impl->locate = std::move (locate);
    impl->create = std::move (create);
    impl->identity = identity;
    impl->rangeUntilMade = range;
    impl->createName = createName;
    impl->undoName = undoName;
    return ParameterHandle (std::move (impl));
}

ParameterHandle makeParameterHandle (te::Plugin* plugin, const juce::String& parameterID, const juce::String& undoName)
{
    return plugin != nullptr ? makeParameterHandle (getSong (plugin->edit), { getDeviceID (*plugin), parameterID }, undoName)
                             : ParameterHandle();
}

ParameterHandle makeParameterHandle (te::Modifier& modifier, const juce::String& parameterID, const juce::String& undoName)
{
    const auto id = DeviceID::fromRaw (getModulatorID (modifier).getRaw());
    return makeParameterHandle (getSong (modifier.edit), { id, parameterID }, undoName, false);
}

ParameterHandle makeMacroHandle (const te::MacroParameter& macro, const juce::String& undoName)
{
    const auto id = DeviceID::fromRaw (getModulatorID (macro).getRaw());
    return makeParameterHandle (getSong (macro.getEdit()), { id, macro.paramID }, undoName, false);
}

Song* findSong (const ParameterHandle& handle)
{
    return handle.getImpl() != nullptr ? handle.getImpl()->song.get() : nullptr;
}

te::AutomatableParameter::Ptr findParameter (const ParameterHandle& handle)
{
    const auto* impl = handle.getImpl();
    return impl != nullptr ? impl->findTracktionParameter (impl->findRef()) : nullptr;
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
    return exists() || (impl != nullptr && impl->create != nullptr && impl->song.get() != nullptr);
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
        return impl->identity == other.impl->identity && impl->ref.parameter == other.impl->ref.parameter;

    return impl->song.get() == other.impl->song.get() && impl->ref == other.impl->ref;
}

ParameterRef ParameterHandle::getRef() const
{
    return impl != nullptr && impl->automatable ? impl->findRef() : ParameterRef();
}

juce::String ParameterHandle::getName() const
{
    const auto description = impl != nullptr ? impl->describe() : std::nullopt;
    return description.has_value() ? description->name : juce::String();
}

juce::NormalisableRange<float> ParameterHandle::getRange() const
{
    if (const auto description = impl != nullptr ? impl->describe() : std::nullopt)
        return description->range;

    return impl != nullptr && impl->create != nullptr ? impl->rangeUntilMade : juce::NormalisableRange<float>();
}

std::optional<float> ParameterHandle::getDefaultValue() const
{
    const auto description = impl != nullptr ? impl->describe() : std::nullopt;
    return description.has_value() ? description->defaultValue : std::nullopt;
}

float ParameterHandle::getValue() const
{
    // While Phazite's engine plays the project, an automated parameter's value is the one it played.
    if (const auto* parameter = impl != nullptr ? impl->find() : nullptr)
        return getCurrentValue (*impl->song.get(), *parameter, true);

    return impl != nullptr && impl->create != nullptr ? impl->rangeUntilMade.start : 0.0f; // the start of the range until made
}

float ParameterHandle::getBaseValue() const
{
    if (const auto* parameter = impl != nullptr ? impl->find() : nullptr)
        return getCurrentValue (*impl->song.get(), *parameter, false);

    return impl != nullptr && impl->create != nullptr ? impl->rangeUntilMade.start : 0.0f; // the start of the range until made
}

juce::String ParameterHandle::getText (float value) const
{
    const auto* parameter = impl != nullptr ? impl->find() : nullptr;
    return parameter != nullptr ? getParameterText (*impl->song.get(), *parameter, value) : juce::String();
}

std::optional<devices::ParameterInfo> ParameterHandle::getInfo() const
{
    const auto description = impl != nullptr ? impl->describe() : std::nullopt;
    return description.has_value() ? description->info : std::nullopt;
}

void ParameterHandle::beginGesture()
{
    if (impl == nullptr || isInGesture())
        return;

    auto* song = impl->song.get();

    if (song == nullptr)
        return;

    const auto found = impl->findRef();

    // A device the first value makes: the gesture's step begins now, and a press without a drag makes nothing. Within
    // another change's gesture (a drag that moves two parameters), it joins that gesture's step.
    if (! found.isValid() && impl->create == nullptr)
        return;

    if (Gesture::isInProgress (*song))
        impl->joined = true;
    else
        impl->gesture = std::make_unique<Gesture> (*song, found.isValid() ? impl->getUndoName (found) : impl->createName);

    if (found.isValid())
        impl->beginRecording (found);
}

void ParameterHandle::setValue (float value)
{
    if (impl == nullptr || impl->song.get() == nullptr)
        return;

    const auto* found = impl->find();

    if (found == nullptr)
    {
        makeAndSet (value);
        return;
    }

    // As found now, which the change can make it find again.
    const auto parameter = *found;
    auto& song = *impl->song.get();

    if (isInGesture())
    {
        setValueSetByHand (song, parameter, value);
        return;
    }

    const UndoStep step (song, impl->getUndoName (parameter.ref));
    setValueSetByHand (song, parameter, value);
}

void ParameterHandle::makeAndSet (float value)
{
    auto* song = impl->song.get();

    if (impl->create == nullptr || song == nullptr)
        return;

    // In the gesture's step, or a step of its own for a value set outside a gesture.
    std::optional<UndoStep> step;

    if (! isInGesture())
        step.emplace (*song, impl->createName);

    const auto made = impl->create (*song);
    const ParameterRef found { made, impl->ref.parameter };

    if (! made.isValid() || ! describeParameter (*song, found).has_value())
        return;

    if (isInGesture())
        impl->beginRecording (found);

    setValueSetByHand (*song, found, value);
}

void ParameterHandle::endGesture()
{
    if (! isInGesture())
        return;

    if (const auto parameter = impl->findTracktionParameter (impl->findRef()))
    {
        parameter->parameterChangeGestureEnd();

        if (auto* session = findPlayingSession (*impl->song.get())) // what it wrote joins the gesture's step
            session->getWriter().gestureEnded (*parameter);
    }

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
