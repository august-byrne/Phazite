// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Parameters.h"
#include "engine/nodes/Channel.h"
#include "internal/DevicePlugin.h"
#include "internal/Devices.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    std::uint64_t readID (const juce::ValueTree& state, const juce::Identifier& property)
    {
        return (std::uint64_t) (juce::int64) state.getProperty (property, (juce::int64) 0);
    }

    // Tracktion's parameter for a plug-in's or a modulator's, which describes it and holds its value until the plug-ins'
    // instances and the modulators are the song's (docs/tracktion-migration.md, T6 steps 4 and 5).
    te::AutomatableParameter::Ptr findTracktionParameter (const Song& song, const ParameterRef& ref)
    {
        auto* edit = findEdit (song);
        return edit != nullptr ? findParameter (*edit, ref) : nullptr;
    }

    bool isVCA (const juce::ValueTree& channel, DeviceID fader)
    {
        return readID (channel, schema::channelVCA) == fader.getRaw();
    }

    std::optional<FoundParameter> found (const ParameterRef& ref, ParameterSource kind, const juce::ValueTree& state,
                                         const devices::ParameterInfo* info = nullptr)
    {
        return FoundParameter { ref, kind, state, info };
    }

    // A parameter whose curve has one point plays that point's value everywhere: the point moves to a value set by hand,
    // in the same step, as a control's change.
    void moveOnePoint (Song& song, const ParameterRef& ref, float value)
    {
        // Not while automation is written by hand (model/playback/internal/AutomationWriter.h), which writes its own points.
        if (auto* session = findSession (song); session != nullptr && session->getWriter().isWriting (ref))
            return;

        if (auto curve = findCurveState (song, ref); curve.getNumChildren() == 1)
            if (auto point = curve.getChild (0); ! juce::exactlyEqual ((float) point[schema::pointValue], value))
                point.setProperty (schema::pointValue, value, &song.getUndoManager());
    }
} // namespace

//==============================================================================
std::optional<FoundParameter> locateParameter (const Song& song, const ParameterRef& ref)
{
    if (! ref.isValid())
        return {};

    // A fader's first, which controls read most.
    if (const auto channel = findChannelOfFader (song, ref.device); channel.isValid())
    {
        const auto isMaster = channel.hasType (schema::MASTER);

        if (isVCA (channel, ref.device))
            return ref.parameter == schema::vcaVolume ? found (ref, ParameterSource::volume, channel) : std::nullopt;

        if (ref.parameter == (isMaster ? schema::masterVolume : schema::faderVolume))
            return found (ref, ParameterSource::volume, channel);

        if (ref.parameter == (isMaster ? schema::masterPan : schema::faderPan))
            return found (ref, ParameterSource::pan, channel);

        return {};
    }

    if (const auto device = findDeviceState (song, ref.device); device.isValid())
    {
        if (isPluginState (device))
            return findTracktionParameter (song, ref) != nullptr ? found (ref, ParameterSource::plugin, device) : std::nullopt;

        // One of Phazite's own; or a device of a type only tests make (Schema.h: deviceType), as Tracktion's plug-in for it
        // describes it while Tracktion's Edit mirrors the song (T6 step 9 makes the tests' types Phazite's own).
        const auto* info = findOwnDevice (device[schema::deviceType].toString().toStdString());

        if (auto* edit = info == nullptr ? findEdit (song) : nullptr)
            if (auto* tests = dynamic_cast<DevicePlugin*> (mirror::findDevice (*edit, ref.device)))
                info = &tests->getDeviceInfo();

        if (info != nullptr)
            if (const auto index = devices::findParameter (*info, ref.parameter.toStdString()); index >= 0)
                return found (ref, ParameterSource::device, device, &info->parameters[(size_t) index]);

        return {};
    }

    if (const auto send = findSendState (song, ref.device); send.isValid())
        return ref.parameter == schema::sendLevelParameter ? found (ref, ParameterSource::send, send) : std::nullopt;

    if (const auto modulator = findModulatorState (song, ModulatorID::fromRaw (ref.device.getRaw())); modulator.isValid())
    {
        // A macro's one parameter has the macro's ID.
        if (modulator.hasType (schema::MACRO))
            return ref.parameter == juce::String (ref.device.getRaw()) ? found (ref, ParameterSource::macro, modulator) : std::nullopt;

        if (const auto kind = getModulatorKind (modulator))
            if (const auto index = devices::findModulatorParameter (*kind, ref.parameter.toStdString()); index >= 0)
                return found (ref, ParameterSource::modulator, modulator, &devices::getModulatorParameters (*kind)[(size_t) index]);

        return {};
    }

    return {};
}

bool isStillThere (const Song& song, const FoundParameter& parameter)
{
    if (! parameter.state.isAChildOf (song.getState()))
        return false;

    // Its element still names it: a folder's fader of the other kind, for one, has another ID.
    const auto id = parameter.ref.device.getRaw();

    switch (parameter.source)
    {
        case ParameterSource::device:
        case ParameterSource::plugin:
            return readID (parameter.state, schema::deviceID) == id;
        case ParameterSource::volume:
        case ParameterSource::pan:
            return readID (parameter.state, schema::channelFader) == id || readID (parameter.state, schema::channelVCA) == id;
        case ParameterSource::send:
            return readID (parameter.state, schema::sendID) == id;
        case ParameterSource::modulator:
        case ParameterSource::macro:
            return readID (parameter.state, schema::modulatorID) == id;
    }

    return false;
}

//==============================================================================
ParameterDescription describeParameter (const Song& song, const FoundParameter& parameter)
{
    const auto& state = parameter.state;
    const auto atZeroDecibels = engine::decibelsToFaderPosition (0.0f);

    switch (parameter.source)
    {
        case ParameterSource::device:
        case ParameterSource::modulator:
        {
            const auto& info = *parameter.info;
            return { parameter.source, toString (info.name), devices::getRange (info), info.defaultValue, info };
        }

        case ParameterSource::volume:
        {
            const auto* name = state.hasType (schema::MASTER) ? "Master volume" : isVCA (state, parameter.ref.device) ? "VCA" : "Volume";
            return { parameter.source, name, { 0.0f, 1.0f }, atZeroDecibels, {} };
        }

        case ParameterSource::pan:
            return { parameter.source, state.hasType (schema::MASTER) ? "Master pan" : "Pan", { -1.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::send:
            return { parameter.source, "Send level", { 0.0f, 1.0f }, atZeroDecibels, {} };

        case ParameterSource::macro:
            return { parameter.source, state[schema::macroName].toString(), { 0.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::plugin:
            if (const auto tracktions = findTracktionParameter (song, parameter.ref))
                return { parameter.source, tracktions->getParameterName(), tracktions->valueRange, tracktions->getDefaultValue(), {} };

            break;
    }

    return { parameter.source, {}, {}, {}, {} };
}

juce::String getParameterText (const Song& song, const FoundParameter& parameter, float value)
{
    switch (parameter.source)
    {
        case ParameterSource::volume:
            return juce::Decibels::toString (engine::faderPositionToDecibels (value) + 0.001f);

        case ParameterSource::send:
            return juce::Decibels::toString (engine::faderPositionToDecibels (value));

        case ParameterSource::pan:
            if (std::abs (value) < 0.001f)
                return "Centre";

            return value < 0.0f ? juce::String (value, 3) + " Left" : "+" + juce::String (value, 3) + " Right";

        case ParameterSource::device:
        case ParameterSource::modulator:
            // A count (one that moves in whole steps) reads as a whole number.
            if (const auto step = parameter.info->step; step.has_value() && juce::exactlyEqual (*step, std::round (*step)))
                return juce::String (juce::roundToInt (value));

            return juce::String (value, 3);

        case ParameterSource::macro:
            return juce::String (value, 3);

        case ParameterSource::plugin:
            if (const auto tracktions = findTracktionParameter (song, parameter.ref))
                return tracktions->valueToString (value);

            break;
    }

    return {};
}

float getValueSetByHand (const Song& song, const FoundParameter& parameter)
{
    const auto& state = parameter.state;

    switch (parameter.source)
    {
        case ParameterSource::device:
        case ParameterSource::modulator:
            return (float) state.getChildWithName (schema::VALUES)
                .getProperty (juce::Identifier (parameter.ref.parameter), parameter.info->defaultValue);

        case ParameterSource::volume:
            return engine::decibelsToFaderPosition (
                (float) state.getProperty (schema::channelVolume, state.hasType (schema::MASTER) ? defaultMasterVolume : 0.0f));

        case ParameterSource::pan:
            return (float) state.getProperty (schema::channelPan, 0.0f);

        case ParameterSource::send:
            return engine::decibelsToFaderPosition ((float) state.getProperty (schema::sendLevel, 0.0f));

        case ParameterSource::macro:
            return (float) state.getProperty (schema::macroValue, 0.0f);

        case ParameterSource::plugin:
            if (const auto tracktions = findTracktionParameter (song, parameter.ref))
                return tracktions->getCurrentExplicitValue();

            break;
    }

    return 0.0f;
}

void setValueSetByHand (Song& song, const FoundParameter& parameter, float value)
{
    const auto& ref = parameter.ref;
    auto* undo = &song.getUndoManager();
    auto state = parameter.state;

    switch (parameter.source)
    {
        case ParameterSource::device:
        {
            const auto stored = juce::jlimit (parameter.info->minimum, parameter.info->maximum, value);
            moveOnePoint (song, ref, stored);
            state.getOrCreateChildWithName (schema::VALUES, undo).setProperty (juce::Identifier (ref.parameter), stored, undo);
            song.reconcile (Song::Part::device, ref.device.getRaw());
            return;
        }

        case ParameterSource::modulator:
        {
            // Nothing moves a modulator's parameters: no curve of one point follows them.
            const auto stored = devices::constrain (*parameter.info, value);
            state.getOrCreateChildWithName (schema::VALUES, undo).setProperty (juce::Identifier (ref.parameter), stored, undo);
            song.reconcile (Song::Part::modulator, ref.device.getRaw());
            return;
        }

        case ParameterSource::volume:
        {
            const auto position = juce::jlimit (0.0f, 1.0f, value);
            moveOnePoint (song, ref, position);
            setChannelVolume (song, getTrackID (state), engine::faderPositionToDecibels (position));
            return;
        }

        case ParameterSource::pan:
        {
            const auto pan = juce::jlimit (-1.0f, 1.0f, value);
            moveOnePoint (song, ref, pan);
            setChannelPan (song, getTrackID (state), pan);
            return;
        }

        case ParameterSource::send:
        {
            const auto position = juce::jlimit (0.0f, 1.0f, value);
            moveOnePoint (song, ref, position);
            setSendLevel (song, ref.device, engine::faderPositionToDecibels (position));
            return;
        }

        case ParameterSource::macro:
        {
            if (const auto stored = juce::jlimit (0.0f, 1.0f, value); juce::exactlyEqual (stored, 0.0f))
                state.removeProperty (schema::macroValue, undo);
            else
                state.setProperty (schema::macroValue, stored, undo);

            song.reconcile (Song::Part::modulator, ref.device.getRaw());
            return;
        }

        case ParameterSource::plugin:
            if (const auto tracktions = findTracktionParameter (song, ref))
                setDeviceValue (*tracktions, value);

            return;
    }
}

float getCurrentValue (const Song& song, const FoundParameter& parameter, bool withModulation)
{
    // Nothing moves a modulator's parameters or a macro's value.
    if (parameter.source == ParameterSource::modulator || parameter.source == ParameterSource::macro)
        return getValueSetByHand (song, parameter);

    // While Phazite's engine plays the song: what it played, or the value set by hand for one that nothing moves (but a
    // modulator's or a macro's, which it doesn't play as parameters).
    if (const auto* session = findPlayingSession (song);
        session != nullptr && parameter.source != ParameterSource::modulator && parameter.source != ParameterSource::macro)
    {
        if (! session->getPlayer().isMoved (parameter.ref))
            return getValueSetByHand (song, parameter);

        if (const auto played = session->getPlayer().getPlayedValue (parameter.ref, withModulation))
            return *played;
    }

    if (const auto tracktions = findTracktionParameter (song, parameter.ref))
        return withModulation ? tracktions->getCurrentValue() : tracktions->getCurrentBaseValue();

    return getValueSetByHand (song, parameter);
}

//==============================================================================
std::optional<ParameterDescription> describeParameter (const Song& song, const ParameterRef& ref)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? std::optional (describeParameter (song, *parameter)) : std::nullopt;
}

juce::String getParameterText (const Song& song, const ParameterRef& ref, float value)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getParameterText (song, *parameter, value) : juce::String();
}

float getValueSetByHand (const Song& song, const ParameterRef& ref)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getValueSetByHand (song, *parameter) : 0.0f;
}

void setValueSetByHand (Song& song, const ParameterRef& ref, float value)
{
    if (const auto parameter = locateParameter (song, ref))
        setValueSetByHand (song, *parameter, value);
}

float getCurrentValue (const Song& song, const ParameterRef& ref, bool withModulation)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getCurrentValue (song, *parameter, withModulation) : 0.0f;
}
} // namespace phazite
