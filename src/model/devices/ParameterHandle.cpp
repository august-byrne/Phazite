// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterHandle.h"
#include "engine/nodes/Channel.h"
#include "internal/ParameterHandle.h"
#include "internal/Parameters.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Json.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedInstance.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace phazite
{
namespace
{
    // A number as it may be typed ("-6", "+3.5", "0.300"); nothing for text that isn't one.
    std::optional<float> readNumber (const juce::String& text)
    {
        const auto t = text.trim();

        if (! t.containsOnly ("+-.0123456789") || ! t.containsAnyOf ("0123456789"))
            return {};

        // Typed, or a plug-in's text; four hundred digits are no number.
        return readFiniteDouble (t).has_value() ? std::optional (readFloat (t)) : std::nullopt;
    }

    // The text without the first of these endings that it has, trimmed, and whether it had one.
    bool dropEnding (juce::String& text, std::initializer_list<const char*> endings)
    {
        for (const auto* ending : endings)
        {
            if (text.endsWith (ending))
            {
                text = text.dropLastCharacters ((int) std::strlen (ending)).trim();
                return true;
            }
        }

        return false;
    }

    // A value as getParameterText() writes it, or as it may be typed in the same units: a fader's decibels ("-6.00 dB",
    // "-inf") as the fader's position, a pan as a fraction, as written ("0.300 Left"), or as a percentage ("30% left", "30 L"),
    // a crossover's hertz ("1.2k"), a choice by its name, any other as a number; a plug-in's not at all, which the plug-in
    // reads itself (ParameterHandle::readText() with a call).
    std::optional<float> readParameterText (const FoundParameter& parameter, const juce::String& typed)
    {
        auto t = typed.trim().toLowerCase();

        switch (parameter.source)
        {
            case ParameterSource::volume:
            case ParameterSource::send:
            case ParameterSource::chainVolume:
            {
                dropEnding (t, { "db" });

                if (t == "-inf" || t == "-infinity")
                    return 0.0f; // silence, the fader's foot

                const auto decibels = readNumber (t);
                return decibels.has_value() ? std::optional (engine::decibelsToFaderPosition (*decibels)) : std::nullopt;
            }

            case ParameterSource::pan:
            case ParameterSource::chainPan:
            {
                if (t == "centre" || t == "center" || t == "c")
                    return 0.0f;

                const auto left = dropEnding (t, { "left", "l" });
                const auto right = ! left && dropEnding (t, { "right", "r" });
                const auto percent = dropEnding (t, { "%" });
                const auto amount = readNumber (t);

                if (! amount.has_value())
                    return {};

                const auto size = juce::jmin (1.0f, std::abs (*amount) / (percent || std::abs (*amount) > 1.0f ? 100.0f : 1.0f));
                return (left || (! right && *amount < 0.0f)) ? -size : size;
            }

            case ParameterSource::crossover:
            {
                const auto kilo = dropEnding (t, { "khz", "k" });

                if (! kilo)
                    dropEnding (t, { "hz" });

                const auto hertz = readNumber (t);
                return hertz.has_value() ? std::optional (*hertz * (kilo ? 1000.0f : 1.0f)) : std::nullopt;
            }

            case ParameterSource::device:
            case ParameterSource::modulator:
            {
                const auto& info = *parameter.info;

                for (size_t i = 0; i < info.choices.size(); ++i)
                    if (t == juce::String (info.choices[i].data(), info.choices[i].size()).toLowerCase())
                        return info.minimum + (float) i;

                return readNumber (t);
            }

            case ParameterSource::macro:
                return readNumber (t);

            case ParameterSource::plugin:
                return {}; // the plug-in reads it (readText() with a call), never Phazite (the review of known bug 166)
        }

        return {};
    }

    // Whether a song's history has moved since this was made, for an answer that comes later than what it answers: an undo or a
    // redo, a gesture's roll-back and a trial put back (each step taken back alone), told by the history once the move is whole
    // (Song.h: HistoryListener), whatever the step changed; or a step begun by hand (countHistoryMoves()). Not what changes the
    // song as it plays (automation written, a take that grows, a plug-in's own change, a state written, a window's place kept),
    // which moves no history: the review of known bug 166, a value typed while the song recorded was dropped.
    struct HistoryWatch final : private Song::HistoryListener
    {
        explicit HistoryWatch (Song& watched)
            : song (&watched),
              moves (countHistoryMoves (watched))
        {
            watched.addHistoryListener (this);
        }

        ~HistoryWatch() override
        {
            if (auto* watched = song.get())
                watched->removeHistoryListener (this);
        }

        bool hasMoved() const { return undone || song == nullptr || countHistoryMoves (*song) != moves; }

    private:
        juce::WeakReference<Song> song;
        const std::uint64_t moves;
        bool undone = false;

        void historyMoved() override { undone = true; }

        JUCE_DECLARE_NON_COPYABLE (HistoryWatch)
    };

    // A value read, kept to a range: none for one that is no number (known bug 181: jlimit() passes NaN on).
    std::optional<float> keepInRange (std::optional<float> value, const juce::NormalisableRange<float>& range)
    {
        if (! value.has_value() || ! std::isfinite (*value))
            return {};

        return juce::jlimit (range.start, range.end, *value);
    }
} // namespace

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
    // (controls read it at each of the display ticker's ticks: docs/archive/performance-audit.md, U5).
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
        return undoName.isNotEmpty() ? undoName : getChangeName (*song.get(), found);
    }

    // A gesture begun: for Phazite's automation writer.
    void beginRecording (const ParameterRef& found) const
    {
        if (auto* session = findPlayingSession (*song.get()))
        {
            session->getWriter().rememberGrab (found); // for Escape (cancelGesture())
            session->getWriter().gestureBegan (found);
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

Song* findSong (const ParameterHandle& handle)
{
    return handle.getImpl() != nullptr ? handle.getImpl()->song.get() : nullptr;
}

ParameterRef findParameterRef (const ParameterHandle& handle)
{
    return handle.getImpl() != nullptr ? handle.getImpl()->findRef() : ParameterRef();
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
    return getText (value, {});
}

juce::String ParameterHandle::getText (float value, std::function<void()> whenKnown) const
{
    const auto* parameter = impl != nullptr ? impl->find() : nullptr;
    return parameter != nullptr ? getParameterText (*impl->song.get(), *parameter, value, std::move (whenKnown)) : juce::String();
}

std::optional<float> ParameterHandle::readText (const juce::String& text) const
{
    const auto* parameter = impl != nullptr ? impl->find() : nullptr;
    const auto value = parameter != nullptr ? readParameterText (*parameter, text) : std::nullopt;
    return keepInRange (value, getRange());
}

bool ParameterHandle::readsTextLater() const
{
    const auto* parameter = impl != nullptr ? impl->find() : nullptr;

    if (parameter == nullptr || parameter->source != ParameterSource::plugin)
        return false;

    auto* instance = findPluginInstance (*impl->song.get(), parameter->ref.device);
    return instance != nullptr && instance->findParameter (parameter->ref.parameter) >= 0;
}

void ParameterHandle::readText (const juce::String& text, std::function<void (std::optional<float>)> then) const
{
    if (then == nullptr)
        return;

    // A plug-in's as the plug-in reads it, asked of its helper without waiting (known bug 166).
    if (const auto* parameter = impl != nullptr ? impl->find() : nullptr;
        parameter != nullptr && parameter->source == ParameterSource::plugin)
    {
        if (auto* instance = findPluginInstance (*impl->song.get(), parameter->ref.device))
        {
            if (const auto index = instance->findParameter (parameter->ref.parameter); index >= 0)
            {
                // Its answer taken only if the song's history hasn't moved since (an undo, a redo, another step) and the
                // parameter is still there (the review of known bug 166: what it would be set on may be other by then).
                // What changes the song as it plays (automation written, a take) leaves it alone.
                auto watch = std::make_shared<HistoryWatch> (*impl->song.get());
                instance->readValue (index, text,
                                     [song = impl->song, wanted = parameter->ref, range = getRange(), then = std::move (then),
                                      watch] (std::optional<float> value)
                                     {
                                         auto* in = song.get();
                                         const auto stillThere = in != nullptr && locateParameter (*in, wanted).has_value();
                                         then (watch->hasMoved() || ! stillThere ? std::nullopt : keepInRange (value, range));
                                     });
                return;
            }
        }
    }

    then (readText (text));
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
    // Nothing for a value that is no number (known bug 181), not even an undo step.
    if (impl == nullptr || impl->song.get() == nullptr || ! std::isfinite (value))
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

    if (auto* song = impl->song.get())
        if (auto* session = findPlayingSession (*song)) // what it wrote joins the gesture's step
            session->getWriter().gestureEnded (impl->findRef());

    if (impl->gesture != nullptr)
    {
        impl->gesture->commit();
        impl->gesture.reset();
    }

    impl->joined = false;
}

void ParameterHandle::cancelGesture()
{
    if (! isInGesture())
        return;

    // Automation being written by hand goes on as it was before the grab, without what the drag recorded: ended as the drag
    // ends, a latch pass kept it, to write as playback stopped, and a write pass wrote all it had into the rolled-back step.
    // Told before the rollback, while the parameter is as the drag found it: what the writer keeps of the drag's time (a
    // part written as the loop went round, or as playback stopped) waits for the rollback, and is a step of its own after it.
    if (auto* song = impl->song.get())
        if (auto* session = findPlayingSession (*song))
            session->getWriter().gestureCancelled (impl->findRef());

    if (impl->gesture != nullptr)
    {
        impl->gesture->rollBack();
        impl->gesture.reset();
    }

    impl->joined = false;
}

bool ParameterHandle::isInGesture() const
{
    return impl != nullptr && (impl->gesture != nullptr || impl->joined);
}
} // namespace phazite
