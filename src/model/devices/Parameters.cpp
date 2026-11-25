// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Parameters.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/Container.h"
#include "internal/Devices.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // A plug-in's parameter in its instance (none while no session plays the song, or the plug-in doesn't load): its
    // instance and its place there.
    std::pair<SandboxedInstance*, int> findPluginParameter (const Song& song, const ParameterRef& ref)
    {
        auto* instance = findPluginInstance (song, ref.device);
        const auto index = instance != nullptr ? instance->findParameter (ref.parameter) : -1;
        return { index >= 0 ? instance : nullptr, index };
    }

    // Whether two of a plug-in's values, from 0 to 1, are the same, to a millionth (as isDefaultValue() takes them).
    bool isSamePluginValue (float a, float b)
    {
        return std::abs (a - b) <= 1.0e-6f;
    }

    // Whether a value is a plug-in's parameter's default, as its instance tells it (none known without one).
    bool isPluginDefault (const Song& song, const ParameterRef& ref, float value)
    {
        const auto [instance, index] = findPluginParameter (song, ref);
        return instance != nullptr && isSamePluginValue (value, instance->getParameterInfos()[(size_t) index].defaultValue);
    }

    // The track's ID a channel is named by in the mixer's functions (model/mixer/internal/Mixer.h): none for the master's.
    TrackID getMixerChannel (const ChannelRef& channel)
    {
        return channel.onMaster ? TrackID() : channel.track;
    }

    // A modulator or a macro of a parameter found (a modulator's parameter, a macro's value), by its ID: a look in modulation's
    // area (model/modulation/internal/Modulation.h), as its FoundParameter holds no element of it.
    std::optional<ModulatorValue> findModulatorOf (const Song& song, const ParameterRef& ref)
    {
        return getModulatorValue (song, ModulatorID::fromRaw (ref.device.getRaw()));
    }

    // Whether a parameter's description is one of a modulator kind's (a FoundParameter's info: the kind it was found of).
    bool isParameterOf (devices::ModulatorKind kind, const devices::ParameterInfo* info)
    {
        const auto parameters = devices::getModulatorParameters (kind);
        return std::any_of (parameters.begin(), parameters.end(), [info] (const devices::ParameterInfo& each) { return &each == info; });
    }

    std::optional<FoundParameter> found (const ParameterRef& ref, ParameterSource kind, const devices::ParameterInfo* info = nullptr)
    {
        return FoundParameter { ref, kind, info };
    }

    // A fader's volume or pan, by its channel (FoundParameter::channel), with no element.
    std::optional<FoundParameter> foundOnChannel (const ParameterRef& ref, ParameterSource kind, ChannelRef channel)
    {
        FoundParameter parameter { ref, kind, nullptr };
        parameter.channel = channel;
        return parameter;
    }

    // A fader's parameter, of the channel whose fader or VCA the reference's device is (none for another device), that
    // channel's VCA given.
    std::optional<FoundParameter> locateOnFader (const ParameterRef& ref, ChannelRef channel, DeviceID vca)
    {
        if (! channel.isValid())
            return {};

        const auto isMaster = channel.onMaster;

        if (vca == ref.device)
            return ref.parameter == schema::vcaVolume ? foundOnChannel (ref, ParameterSource::volume, channel) : std::nullopt;

        if (ref.parameter == (isMaster ? schema::masterVolume : schema::faderVolume))
            return foundOnChannel (ref, ParameterSource::volume, channel);

        if (ref.parameter == (isMaster ? schema::masterPan : schema::faderPan))
            return foundOnChannel (ref, ParameterSource::pan, channel);

        return {};
    }

    // A chain's place among its container's, and how many chains it has; and whether it has a crossover: a band's, but the
    // first's (which starts at the bottom), in a container split into bands. Each by the chain's ID, looks in the devices' area.
    std::pair<int, int> getChainPlace (const Song& song, DeviceID chain)
    {
        return { getChainIndex (song, chain), (int) getChains (song, getChainContainer (song, chain)).size() };
    }

    bool hasCrossover (const Song& song, DeviceID chain)
    {
        return getContainerSplit (song, getChainContainer (song, chain)) == ContainerSplit::bands && getChainIndex (song, chain) > 0;
    }

    // A chain's crossover, its own or its place's default; and the ones it stays between: the chain's before it (the bottom,
    // for the second chain) and the one's after it (the top, for the last).
    juce::Range<float> getCrossoverRoom (const Song& song, DeviceID chain)
    {
        const auto chains = getChains (song, getChainContainer (song, chain));
        const auto index = getChainIndex (song, chain);
        const auto below = index > 1 ? getChainCrossover (song, chains[(size_t) index - 1]) : lowestCrossover;
        const auto above = index + 1 < (int) chains.size() ? getChainCrossover (song, chains[(size_t) index + 1]) : highestCrossover;
        return { below, std::max (below, above) };
    }

    // A parameter's description among one of Phazite's own device's, by the device's ID: its index there, -1 for a device the
    // song hasn't, of another form, or one whose type's description holds no such parameter (a type changed since it was
    // found).
    int findParameterIndex (const Song& song, DeviceID device, const devices::ParameterInfo* info)
    {
        const auto* value = findDevice (song, device);
        const auto* own = value != nullptr && value->form == DeviceValue::Form::own
                              ? findOwnDevice (std::string_view (value->type.toRawUTF8()))
                              : nullptr;

        const std::less<const devices::ParameterInfo*> before;

        if (own == nullptr || info == nullptr || own->parameters.empty() || before (info, own->parameters.data())
            || ! before (info, own->parameters.data() + own->parameters.size()))
            return -1;

        return (int) (info - own->parameters.data());
    }

    juce::String describeFrequency (float hertz)
    {
        return hertz < 1000.0f ? juce::String (juce::roundToInt (hertz)) + " Hz"
                               : juce::String (hertz / 1000.0f, hertz < 10000.0f ? 2 : 1) + " kHz";
    }

    // The same as hasCrossover() in a value of the song's devices given (a value of their own properties alone will do).
    bool hasCrossoverIn (const SongDevices& devices, DeviceID chain)
    {
        const auto* value = devices.find (chain);
        const auto* container =
            value != nullptr && value->form == DeviceValue::Form::chain ? devices.find (value->place.container) : nullptr;

        if (container == nullptr || container->form != DeviceValue::Form::container || container->split != ContainerSplit::bands)
            return false;

        const auto* chains = devices.chains.find (container->id.getRaw());
        return chains != nullptr && ! chains->empty() && chains->front() != chain
               && std::find (chains->begin(), chains->end(), chain) != chains->end();
    }

    // locateParameter()'s look, with how a modulator or a macro is found by its ID, whether a send of an ID is there, how a
    // device, a container or a chain is found by its ID (a pointer valid while the look lasts, nullptr for none), whether a
    // chain has a crossover, how a fader's parameter is found, and whether a plug-in has a parameter (the song's areas looked
    // at, or a file's tree's read once: locateParameterIn()).
    template <typename FindModulator, typename HasSend, typename FindDevice, typename HasCrossover, typename OnFader, typename PluginHas>
    std::optional<FoundParameter> locate (const ParameterRef& ref, FindModulator&& findModulatorOf, HasSend&& hasSend,
                                          FindDevice&& findDeviceOf, HasCrossover&& crossover, OnFader&& onFaderOf, PluginHas&& pluginHas)
    {
        if (! ref.isValid())
            return {};

        // A modulator's or a macro's first: a look in modulation's area by its ID, which allocates nothing, before the other
        // looks, which a macro's turn (the automation's watcher asks at each step) would otherwise pay for every track. A
        // reference to a device, a chain or a fader looks there for an ID no modulator has (the song's IDs are each one
        // item's), and finds none whatever the area holds.
        if (const auto modulator = findModulatorOf (ref))
        {
            // A macro's one parameter has the macro's ID.
            if (modulator->isMacro())
                return ref.parameter == juce::String (ref.device.getRaw()) ? found (ref, ParameterSource::macro) : std::nullopt;

            if (const auto index = devices::findModulatorParameter (*modulator->kind, ref.parameter.toStdString()); index >= 0)
                return found (ref, ParameterSource::modulator, &devices::getModulatorParameters (*modulator->kind)[(size_t) index]);

            return {};
        }

        // A send's level, by the send's ID: a look in the sends' area. Its level is a send's one parameter (where it goes and
        // whether it is before the fader are no parameters), and no chain or modulator has a send's ID, so only a reference
        // to that parameter asks the sends.
        if (ref.parameter == schema::sendLevelParameter)
            if (hasSend (ref.device))
                return found (ref, ParameterSource::send);

        // A fader's, which controls read most, by its channel.
        if (auto onFader = onFaderOf (ref))
            return onFader;

        // A device's, a plug-in's or a chain's: a look in the devices' area by its ID.
        const auto* value = findDeviceOf (ref.device);

        if (value == nullptr)
            return {};

        switch (value->form)
        {
            case DeviceValue::Form::plugin:
            {
                // One its instance has, or one the song keeps a value of (a plug-in that doesn't load keeps what refers to
                // it).
                return pluginHas (ref) ? found (ref, ParameterSource::plugin) : std::nullopt;
            }

            case DeviceValue::Form::own:
            {
                // One of Phazite's own (or of a type a test registered: model/devices/internal/Devices.h).
                if (const auto* info = findOwnDevice (std::string_view (value->type.toRawUTF8())))
                    if (const auto index = devices::findParameter (*info, ref.parameter.toStdString()); index >= 0)
                        return found (ref, ParameterSource::device, &info->parameters[(size_t) index]);

                return {};
            }

            case DeviceValue::Form::chain:
            {
                // A container's chain's volume and pan, and a band's crossover.
                if (ref.parameter == schema::chainVolumeParameter)
                    return found (ref, ParameterSource::chainVolume);

                if (ref.parameter == schema::chainPanParameter)
                    return found (ref, ParameterSource::chainPan);

                if (ref.parameter == schema::crossoverParameter && crossover (ref.device))
                    return found (ref, ParameterSource::crossover, &engine::crossoverInfo);

                return {};
            }

            case DeviceValue::Form::container:
                break;
        }

        return {};
    }
} // namespace

//==============================================================================
std::optional<FoundParameter> locateParameter (const Song& song, const ParameterRef& ref)
{
    return locate (
        ref, [&song] (const ParameterRef& modulator) { return findModulatorOf (song, modulator); },
        [&song] (DeviceID send) { return getSendValue (song, send).has_value(); }, [&song] (DeviceID id) { return findDevice (song, id); },
        [&song] (DeviceID chain) { return hasCrossover (song, chain); },
        [&song] (const ParameterRef& onFader)
        {
            const auto channel = findChannelOfFader (song, onFader.device);
            return locateOnFader (onFader, channel, channel.isValid() ? getChannelVCA (song, channel) : DeviceID());
        },
        [&song] (const ParameterRef& plugin)
        {
            // One its instance has, or one the song keeps a value of (a plug-in that doesn't load keeps what refers to it).
            return findPluginParameter (song, plugin).first != nullptr
                   || findPluginValue (song, plugin.device, plugin.parameter).has_value();
        });
}

std::optional<FoundParameter> locateParameterIn (const juce::ValueTree& song, const ParameterRef& ref, const SongDevices& devices,
                                                 const SongTracks& tracks, const std::vector<TrackID>& order, const SongSends& sends)
{
    // A fader's among the tracks given, in their order, then the master's; its channel's VCA from the same.
    const auto onFaderOf = [&tracks, &order] (const ParameterRef& onFader)
    {
        const auto channel = findChannelOfFaderIn (tracks, order, onFader.device);
        const auto* value = channel.onMaster ? &tracks.master : tracks.find (channel.track);
        return locateOnFader (onFader, channel, value != nullptr ? value->vca : DeviceID());
    };

    // A modulator's element of the ID read alone (never every modulator of the song for each curve), a send among the sends
    // given; and no plug-in's parameter (no instance plays a file's song; its curves and links are kept by the loader as their
    // elements say they are a plug-in's).
    return locate (
        ref,
        [&song] (const ParameterRef& modulator) { return readModulatorValueIn (song, ModulatorID::fromRaw (modulator.device.getRaw())); },
        [&sends] (DeviceID send) { return sends.find (send) != nullptr; }, [&devices] (DeviceID id) { return devices.find (id); },
        [&devices] (DeviceID chain) { return hasCrossoverIn (devices, chain); }, onFaderOf, [] (const ParameterRef&) { return false; });
}

bool isStillThere (const Song& song, const FoundParameter& parameter)
{
    // A modulator's or a macro's by its ID, still of the kind it was found of (a merge's choice can give it another).
    if (parameter.source == ParameterSource::modulator || parameter.source == ParameterSource::macro)
    {
        const auto modulator = findModulatorOf (song, parameter.ref);

        if (! modulator.has_value())
            return false;

        return parameter.source == ParameterSource::macro ? modulator->isMacro()
                                                          : modulator->kind.has_value() && isParameterOf (*modulator->kind, parameter.info);
    }

    // A send's level by its send's ID, a look in the sends' area.
    if (parameter.source == ParameterSource::send)
        return getSendValue (song, parameter.ref.device).has_value();

    // A fader's by its channel, which still has it as its fader or its VCA: a folder's fader of the other kind, for one, has
    // another ID. One lookup.
    if (parameter.source == ParameterSource::volume || parameter.source == ParameterSource::pan)
        return isFaderOf (song, parameter.channel, parameter.ref.device);

    // A device's, a plug-in's or a chain's by its ID, a look in the devices' area, still of the form it was found of: one of
    // Phazite's own still of the type whose parameter it found (none allocated), a chain's crossover still a band's.
    const auto* value = findDevice (song, parameter.ref.device);

    if (value == nullptr)
        return false;

    switch (parameter.source)
    {
        case ParameterSource::device:
            return findParameterIndex (song, parameter.ref.device, parameter.info) >= 0;
        case ParameterSource::plugin:
            return value->isPlugin();
        case ParameterSource::volume:
        case ParameterSource::pan:
        case ParameterSource::send:
        case ParameterSource::modulator:
        case ParameterSource::macro:
            break; // above
        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
            return value->form == DeviceValue::Form::chain;
        case ParameterSource::crossover:
            return value->form == DeviceValue::Form::chain && hasCrossover (song, parameter.ref.device);
    }

    return false;
}

//==============================================================================
ParameterDescription describeParameter (const Song& song, const FoundParameter& parameter)
{
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
            // The master's starts a little down (defaultMasterVolume), where a double-click sets it back.
            const auto isMaster = parameter.channel.onMaster;
            const auto* name = isMaster                                                          ? "Master volume"
                               : getChannelVCA (song, parameter.channel) == parameter.ref.device ? "VCA"
                                                                                                 : "Volume";
            return { parameter.source,
                     name,
                     { 0.0f, 1.0f },
                     isMaster ? engine::decibelsToFaderPosition (defaultMasterVolume) : atZeroDecibels,
                     {} };
        }

        case ParameterSource::pan:
            return { parameter.source, parameter.channel.onMaster ? "Master pan" : "Pan", { -1.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::send:
            return { parameter.source, "Send level", { 0.0f, 1.0f }, atZeroDecibels, {} };

        case ParameterSource::macro:
        {
            const auto macro = findModulatorOf (song, parameter.ref);
            return { parameter.source, macro.has_value() ? macro->name : juce::String(), { 0.0f, 1.0f }, 0.0f, {} };
        }

        case ParameterSource::chainVolume:
            return { parameter.source, "Volume", { 0.0f, 1.0f }, atZeroDecibels, {} };

        case ParameterSource::chainPan:
            return { parameter.source, "Pan", { -1.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::crossover:
        {
            const auto [index, count] = getChainPlace (song, parameter.ref.device);
            return { parameter.source, "Crossover", devices::getRange (engine::crossoverInfo), getDefaultCrossover (index, count),
                     engine::crossoverInfo };
        }

        case ParameterSource::plugin:
        {
            // From 0 to 1, in its steps if it says it has some, as the plug-in told it as it opened.
            if (const auto [instance, index] = findPluginParameter (song, parameter.ref); instance != nullptr)
            {
                const auto& info = instance->getParameterInfos()[(size_t) index];
                const auto step = info.numSteps > 1 ? 1.0f / (float) (info.numSteps - 1) : 0.0f;
                return { parameter.source, info.name, { 0.0f, 1.0f, step }, info.defaultValue, {} };
            }

            return { parameter.source, parameter.ref.parameter, { 0.0f, 1.0f }, {}, {} };
        }
    }

    return { parameter.source, {}, {}, {}, {} };
}

juce::String getParameterText (const Song& song, const FoundParameter& parameter, float value, std::function<void()> whenKnown)
{
    switch (parameter.source)
    {
        case ParameterSource::volume:
        case ParameterSource::chainVolume:
            return juce::Decibels::toString (engine::faderPositionToDecibels (value) + 0.001f);

        case ParameterSource::send:
            return juce::Decibels::toString (engine::faderPositionToDecibels (value));

        case ParameterSource::crossover:
            return describeFrequency (value);

        case ParameterSource::pan:
        case ParameterSource::chainPan:
            if (std::abs (value) < 0.001f)
                return "Centre";

            return value < 0.0f ? juce::String (value, 3) + " Left" : "+" + juce::String (value, 3) + " Right";

        case ParameterSource::device:
        case ParameterSource::modulator:
            // A choice reads as its name ("Sine"), and a count (one that moves in whole steps) as a whole number.
            if (const auto& info = *parameter.info; info.unit == devices::Unit::choice && ! info.choices.empty())
                return toString (
                    info.choices[(size_t) juce::jlimit (0, (int) info.choices.size() - 1, juce::roundToInt (value - info.minimum))]);

            if (const auto step = parameter.info->step; step.has_value() && juce::exactlyEqual (*step, std::round (*step)))
                return juce::String (juce::roundToInt (value));

            return juce::String (value, 3);

        case ParameterSource::macro:
            return juce::String (value, 3);

        case ParameterSource::plugin:
            if (const auto [instance, index] = findPluginParameter (song, parameter.ref); instance != nullptr)
                return instance->describeValue (index, value, std::move (whenKnown));

            return juce::String (value, 2);
    }

    return {};
}

float getValueSetByHand (const Song& song, const FoundParameter& parameter)
{
    switch (parameter.source)
    {
        case ParameterSource::device:
        {
            // By the parameter's index among its device's description's, a look in the devices' area and one in its values' map:
            // nothing allocated at a tick. The parameter's default for a device it is no parameter of now.
            const auto* value = findDevice (song, parameter.ref.device);
            const auto index = findParameterIndex (song, parameter.ref.device, parameter.info);
            return value != nullptr && index >= 0 ? value->getParameterValue (index) : parameter.info->defaultValue;
        }

        case ParameterSource::modulator:
        {
            // Its value's, by the parameter's place among its kind's; the parameter's default for one it hasn't.
            const auto modulator = findModulatorOf (song, parameter.ref);

            if (! modulator.has_value() || modulator->isMacro())
                return parameter.info->defaultValue;

            const auto index = devices::findModulatorParameter (*modulator->kind, parameter.ref.parameter.toStdString());
            return index >= 0 ? modulator->getParameterValue (index) : parameter.info->defaultValue;
        }

        case ParameterSource::volume:
            // Its channel's, by its ID, the master's by its flag (model/mixer/internal/Mixer.h: getChannelVolume()).
            return engine::decibelsToFaderPosition (getChannelVolume (song, getMixerChannel (parameter.channel)));

        case ParameterSource::pan:
            return getChannelPan (song, getMixerChannel (parameter.channel));

        case ParameterSource::send:
        {
            const auto send = getSendValue (song, parameter.ref.device); // a look in the sends' area, by its ID
            return engine::decibelsToFaderPosition (send.has_value() ? send->level : 0.0f);
        }

        case ParameterSource::macro:
        {
            const auto macro = findModulatorOf (song, parameter.ref);
            return macro.has_value() ? macro->value : 0.0f;
        }

        case ParameterSource::chainVolume:
            return engine::decibelsToFaderPosition (getChainVolume (song, parameter.ref.device));

        case ParameterSource::chainPan:
            return getChainPan (song, parameter.ref.device);

        case ParameterSource::crossover:
            return getChainCrossover (song, parameter.ref.device);

        case ParameterSource::plugin:
            // The song's, where it sets one (a look in the store by its ID: model/plugins/internal/DeviceStore.h); else its
            // instance's.
            if (const auto stored = findPluginValue (song, parameter.ref.device, parameter.ref.parameter))
                return *stored;

            if (const auto [instance, index] = findPluginParameter (song, parameter.ref); instance != nullptr)
                return instance->getParameterValue (index);

            break;
    }

    return 0.0f;
}

void setValueSetByHand (Song& song, const FoundParameter& parameter, float value)
{
    // A value typed, or a plug-in's, that is no finite number changes nothing: juce::jlimit() would pass a NaN on (known
    // bug 148).
    if (! std::isfinite (value))
        return;

    const auto& ref = parameter.ref;
    auto told = value;

    switch (parameter.source)
    {
        case ParameterSource::device:
        {
            // As the song holds it: within its range and on its steps (SavedValues.h: keepHeldValue()), through the devices'
            // setter of one value by the parameter's index (model/mixer/internal/Mixer.h: setDeviceValue()); and so on its steps
            // to a curve's lone point too, and to what is told of it.
            told = devices::constrain (*parameter.info, value);
            moveOnlyPoint (song, ref, told);
            setDeviceValue (song, ref.device, findParameterIndex (song, ref.device, parameter.info), told);
            break;
        }

        case ParameterSource::modulator:
        {
            // Nothing moves a modulator's parameters: no curve of one point follows them. Its value, through modulation's code.
            told = devices::constrain (*parameter.info, value);
            setModulatorValueSetByHand (song, ModulatorID::fromRaw (ref.device.getRaw()), *parameter.info, told);
            break;
        }

        case ParameterSource::plugin:
        {
            // The song's value of the parameter, in the element found (model/plugins/internal/DeviceStore.h), which its
            // instance takes (model/plugins/internal/PluginInstances.h), made holding the value the plug-in has, so that an
            // undo takes it back there; none for the value it has already.
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);

            if (! findPluginValue (song, ref.device, ref.parameter).has_value())
            {
                if (const auto held = getValueSetByHand (song, parameter); ! isSamePluginValue (held, told))
                    setPluginValueSetByHand (song, ref.device, ref.parameter, told, held);

                break;
            }

            setPluginValueSetByHand (song, ref.device, ref.parameter, told, told);

            // One set back to the parameter's default goes, as Phazite's own devices keep none at their defaults
            // (model/mixer/internal/Mixer.h: setDeviceValue()): the plug-in holds the value then, which its settings hold
            // once the next save stores them. A change outside the song's history, so that the project saves it; the
            // song's, not the plug-in's own, which the versions would take its stored settings for a change of
            // (model/base/Song.h).
            if (isPluginDefault (song, ref, told))
            {
                removePluginValueSetByHand (song, ref.device, ref.parameter);
                song.markChangedOutsideHistory();
            }

            break;
        }

        case ParameterSource::volume:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChannelVolume (song, getMixerChannel (parameter.channel), engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::pan:
        {
            told = juce::jlimit (-1.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChannelPan (song, getMixerChannel (parameter.channel), told);
            break;
        }

        case ParameterSource::send:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setSendLevel (song, ref.device, engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::macro:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            setMacroValue (song, ModulatorID::fromRaw (ref.device.getRaw()), told);
            break;
        }

        case ParameterSource::chainVolume:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChainVolume (song, ref.device, engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::chainPan:
        {
            told = juce::jlimit (-1.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChainPan (song, ref.device, told);
            break;
        }

        case ParameterSource::crossover:
        {
            // Between its neighbours' crossovers: a band can't pass the one beside it.
            told = getCrossoverRoom (song, ref.device).clipValue (juce::jlimit (lowestCrossover, highestCrossover, value));
            moveOnlyPoint (song, ref, told);
            setChainCrossover (song, ref.device, told);
            break;
        }
    }

    song.tellValueSetByHand (ref, told, false);
}

float getCurrentValue (const Song& song, const FoundParameter& parameter, bool withModulation)
{
    // Nothing moves a modulator's parameters or a macro's value.
    if (parameter.source == ParameterSource::modulator || parameter.source == ParameterSource::macro)
        return getValueSetByHand (song, parameter);

    // While Phazite's engine plays the song: what it played, or the value set by hand for one that nothing moves.
    if (const auto* session = findPlayingSession (song))
    {
        if (! session->getPlayer().isMoved (parameter.ref))
            return getValueSetByHand (song, parameter);

        if (const auto played = session->getPlayer().getPlayedValue (parameter.ref, withModulation))
            return *played;
    }

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

std::optional<devices::Travel> getControlTravel (const Song& song, const ParameterRef& ref)
{
    if (! ref.isValid())
        return {};

    // A plug-in's from 0 to 1, in the steps it says it has once it has loaded (describeParameter()).
    if (isPlugin (song, ref.device))
    {
        const auto description = describeParameter (song, ref);
        return devices::Travel { .step = description.has_value() ? description->range.interval : 0.0f };
    }

    if (const auto* chain = ref.parameter == schema::crossoverParameter ? findDevice (song, ref.device) : nullptr;
        chain != nullptr && chain->form == DeviceValue::Form::chain)
        return devices::getTravel (engine::crossoverInfo);

    const auto found = locateParameter (song, ref);

    if (! found.has_value())
        return {};

    if (found->info != nullptr)
        return devices::getTravel (*found->info);

    // The mixer's controls and a macro: linear, from their ranges' starts to their ends.
    const auto range = describeParameter (song, *found).range;
    return devices::Travel { range.start, range.end };
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

juce::String getChangeName (const Song& song, const ParameterRef& ref)
{
    const auto parameter = locateParameter (song, ref);

    if (! parameter.has_value())
        return {};

    switch (parameter->source)
    {
        case ParameterSource::volume:
            return parameter->channel.onMaster ? "Change Master Volume" : "Change Volume";

        case ParameterSource::pan:
            return "Change Pan";

        case ParameterSource::send:
            return "Change Send Level";

        case ParameterSource::macro:
            return "Change " + getModulatorName (song, ModulatorID::fromRaw (ref.device.getRaw()));

        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
        case ParameterSource::crossover:
        case ParameterSource::device:
        case ParameterSource::plugin:
        case ParameterSource::modulator:
            break;
    }

    return "Change " + getParameterName (song, ref);
}

ManyValuesSetByHand::ManyValuesSetByHand (const Song& song)
{
    if (auto* session = findSession (song))
    {
        writer = &session->getWriter();
        writer->beginManyValues();
    }
}

ManyValuesSetByHand::~ManyValuesSetByHand()
{
    if (writer != nullptr)
        writer->endManyValues();
}

float getCurrentValue (const Song& song, const ParameterRef& ref, bool withModulation)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getCurrentValue (song, *parameter, withModulation) : 0.0f;
}
} // namespace phazite
