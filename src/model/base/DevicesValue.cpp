// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/DevicesValue.h"
#include "devices/drums/DrumSampler.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "model/base/Numbers.h"
#include "model/base/Song.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/internal/Devices.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> devicesRead { 0 };     // countDevicesReadAsValues(): a count alone, which orders nothing
    std::atomic<std::uint64_t> entriesCompared { 0 }; // countDeviceEntriesCompared(): the same

    // Two entries of a device's values or sounds alike, each pair compared counted: a value's number exactly (immer's own ==
    // would compare floats with ==), a sound's text.
    struct SameEntry
    {
        template <typename Entry>
        bool operator() (const Entry& a, const Entry& b) const
        {
            entriesCompared.fetch_add (1, std::memory_order_relaxed);

            if constexpr (std::is_floating_point_v<typename Entry::second_type>)
                return a.first == b.first && juce::exactlyEqual (a.second, b.second);
            else
                return a.first == b.first && a.second == b.second;
        }
    };

    // Whether two of a device's maps (its values, its sounds) hold the same entries: the same map at once (immer: the same
    // root), else by the nodes where they differ alone, those they share skipped (immer's equality, as a map of the same
    // entries has one shape whatever made it): a value set on a device of thousands compares a few entries, not every one.
    template <typename Map>
    bool holdSameEntries (const Map& a, const Map& b)
    {
        return a.identity() == b.identity() || a.impl().template equals<SameEntry> (b.impl());
    }

    using IDList = immer::flex_vector<DeviceID>;
    using Lists = immer::map<std::uint64_t, IDList>;
    using Form = DeviceValue::Form;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // The description of one of Phazite's own device types (or a type a test registered), by a type as an element holds it;
    // nullptr for another. Without a copy of the type's text.
    const devices::DeviceInfo* findInfo (const juce::String& type)
    {
        return type.isEmpty() ? nullptr : findOwnDevice (std::string_view (type.toRawUTF8()));
    }

    // An ID an element holds or names, as readID() reads one (a number or text): a number's own value, with no text made of it
    // (the compile reads every device's ID and sidechain at each plan, the selection at each check, a control at each tick).
    std::uint64_t readIDOf (const juce::var& held)
    {
        return held.isInt() || held.isInt64() ? (std::uint64_t) (juce::int64) held : readID (held);
    }

    // A number as the song reads one (SongText.h: readValueNumber()), or `otherwise` for none and what is no finite number.
    double readNumber (const juce::var& held, double otherwise)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? *number : otherwise;
    }

    // A number as the song holds it: by its description, within its range (SongText.h: readDescribed()), `otherwise` for none
    // and for what is no finite number, which the loader takes away (Project.h: makeConsistent(), keepNumbers()).
    double readHeldNumber (const juce::var& held, const schema::PropertyDescription& property, double otherwise)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? readDescribed (held, property) : otherwise;
    }

    // A whole number as Phazite's code always read one (an int truncates it), and as the song holds it (within its range,
    // made whole, as the loader makes it: keepInRange()).
    int readWhole (const juce::var& held, int otherwise)
    {
        return held.isVoid() ? otherwise : toWhole<int> (readNumber (held, (double) otherwise), otherwise);
    }

    int readHeldWhole (const juce::var& held, const schema::PropertyDescription& property, int otherwise)
    {
        return roundToWhole<int> (readHeldNumber (held, property, (double) otherwise), otherwise);
    }

    // A chain's crossover as the song holds it: within its range, none for none and for what is no finite number.
    std::optional<float> readHeldCrossover (const juce::var& held)
    {
        const auto number = readValueNumber (held);

        if (! number.has_value() || ! std::isfinite (*number))
            return std::nullopt;

        return (float) readDescribed (held, schema::chainCrossover);
    }

    // A container's split, as its words name it (Schema.h: containerSplit); layers for none and any other word.
    ContainerSplit readSplitWord (const juce::var& word)
    {
        const auto split = word.toString();
        return split == schema::splitBands       ? ContainerSplit::bands
               : split == schema::splitMidSide   ? ContainerSplit::midSide
               : split == schema::splitLeftRight ? ContainerSplit::leftRight
                                                 : ContainerSplit::layers;
    }

    // A split's word as a value, made once (a text made for each container the loader looks at would allocate); none for
    // layers, which is written as none.
    const juce::var* getSplitWord (ContainerSplit split)
    {
        static const juce::var bands (schema::splitBands), midSide (schema::splitMidSide), leftRight (schema::splitLeftRight);

        switch (split)
        {
            case ContainerSplit::bands:
                return &bands;
            case ContainerSplit::midSide:
                return &midSide;
            case ContainerSplit::leftRight:
                return &leftRight;
            case ContainerSplit::layers:
                break;
        }

        return nullptr;
    }

    // A plug-in's type (Schema.h: deviceIsPlugin) as a value, made once.
    const juce::var& getPluginType()
    {
        static const juce::var type (schema::deviceIsPlugin);
        return type;
    }

    //==============================================================================
    // The list of what is at a place: 2 a chain's, 1 a container's, 0 its channel's; and its key there.
    int getListKind (const DevicePlace& place)
    {
        return place.chain.isValid() ? 2 : place.container.isValid() ? 1 : 0;
    }

    std::uint64_t getChannelKey (const DevicePlace& place)
    {
        return place.onMaster ? 0 : place.track.getRaw();
    }

    std::uint64_t getListKey (const DevicePlace& place)
    {
        switch (getListKind (place))
        {
            case 2:
                return place.chain.getRaw();
            case 1:
                return place.container.getRaw();
            default:
                break;
        }

        return getChannelKey (place);
    }

    Lists& getLists (SongDevices& devices, const DevicePlace& place)
    {
        switch (getListKind (place))
        {
            case 2:
                return devices.chainDevices;
            case 1:
                return devices.chains;
            default:
                break;
        }

        return devices.channels;
    }

    bool isSameList (const DevicePlace& a, const DevicePlace& b)
    {
        return getListKind (a) == getListKind (b) && getListKey (a) == getListKey (b) && (getListKind (a) != 0 || a.onMaster == b.onMaster);
    }

    SongDevices withoutInList (SongDevices devices, const DevicePlace& place, DeviceID id)
    {
        auto& lists = getLists (devices, place);
        const auto key = getListKey (place);

        if (const auto* list = lists.find (key))
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                if ((*list)[i] == id)
                {
                    const auto rest = list->erase (i);
                    lists = rest.empty() ? lists.erase (key) : lists.set (key, rest);
                    break;
                }
            }
        }

        return devices;
    }

    SongDevices withInList (SongDevices devices, const DevicePlace& place, DeviceID id, int index)
    {
        auto& lists = getLists (devices, place);
        const auto key = getListKey (place);
        const auto* found = lists.find (key);
        auto list = found != nullptr ? *found : IDList();
        list = juce::isPositiveAndNotGreaterThan (index, (int) list.size()) ? list.insert ((size_t) index, id) : list.push_back (id);
        lists = lists.set (key, std::move (list));
        return devices;
    }

    //==============================================================================
    // The SOUNDS properties of a type, in their order (getSoundPropertiesOf()), and each one's index among them by the
    // address of its name, which every juce::Identifier of one name shares (JUCE's string pool).
    struct SoundTable
    {
        std::vector<juce::Identifier> properties {};
        std::unordered_map<const void*, int> indices {};

        explicit SoundTable (std::vector<juce::Identifier> list)
            : properties (std::move (list))
        {
            for (size_t i = 0; i < properties.size(); ++i)
                indices.emplace (properties[i].getCharPointer().getAddress(), (int) i);
        }
    };

    const SoundTable* findSoundTable (std::string_view typeID)
    {
        using devices::DrumSampler;
        using devices::Sampler;
        using devices::Synth;

        if (typeID == std::string_view (DrumSampler::typeID))
        {
            static const SoundTable drums (
                []
                {
                    std::vector<juce::Identifier> list;

                    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
                    {
                        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
                        {
                            list.push_back (schema::getPadSound (pad, layer));
                            list.push_back (schema::getPadSoundFile (pad, layer));
                        }
                    }

                    return list;
                }());
            return &drums;
        }

        if (typeID == std::string_view (Synth::typeID))
        {
            static const SoundTable synth (
                []
                {
                    std::vector<juce::Identifier> list;

                    for (int osc = 0; osc < Synth::numOscillators; ++osc)
                    {
                        list.push_back (schema::getOscillatorWavetable (osc));
                        list.push_back (schema::getOscillatorWavetableFile (osc));
                    }

                    return list;
                }());
            return &synth;
        }

        if (typeID == std::string_view (Sampler::typeID))
        {
            static const SoundTable sampler (std::vector<juce::Identifier> { schema::samplerSample, schema::samplerSampleFile,
                                                                             schema::samplerSlices, schema::samplerSlicesRemoved });
            return &sampler;
        }

        return nullptr;
    }

    //==============================================================================
    // Each parameter of a description whose value VALUES hold, by its index, as one of the readers takes it.
    template <typename Read>
    immer::map<std::uint32_t, float> readValues (const juce::ValueTree& device, const devices::DeviceInfo& info, Read&& read)
    {
        const auto saved = device.getChildWithName (schema::VALUES);
        auto values = immer::map<std::uint32_t, float>().transient();

        for (int i = 0; i < saved.getNumProperties(); ++i)
        {
            const auto name = saved.getPropertyName (i);

            if (const auto index = devices::findParameter (info, std::string_view (name.getCharPointer().getAddress())); index >= 0)
                if (const auto value = read (info.parameters[(size_t) index], saved[name]))
                    values.set ((std::uint32_t) index, *value);
        }

        return values.persistent();
    }

    // Each sound SOUNDS hold of a property of the type's, but an empty one, by its index.
    immer::map<std::uint16_t, juce::String> readSounds (const juce::ValueTree& device, std::string_view typeID)
    {
        auto sounds = immer::map<std::uint16_t, juce::String>().transient();
        const auto* table = findSoundTable (typeID);
        const auto saved = device.getChildWithName (schema::SOUNDS);

        if (table == nullptr || ! saved.isValid())
            return sounds.persistent();

        for (int i = 0; i < saved.getNumProperties(); ++i)
        {
            const auto name = saved.getPropertyName (i);

            if (const auto found = table->indices.find (name.getCharPointer().getAddress()); found != table->indices.end())
                if (const auto sound = saved[name].toString(); sound.isNotEmpty())
                    sounds.set ((std::uint16_t) found->second, sound);
        }

        return sounds.persistent();
    }

    //==============================================================================
    // The conversion's readers of one field of one element, as readDeviceValue() reads it (DevicesValue.h: the public
    // ones read the others), each of an element that isn't one it is on read as one without it.
    bool readDeviceOff (const juce::ValueTree& element)
    {
        return (element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER)) && (bool) element[schema::deviceOff];
    }

    TrackID readDeviceSidechain (const juce::ValueTree& device)
    {
        return device.hasType (schema::DEVICE) ? TrackID::fromRaw (readIDOf (device[schema::deviceSidechain])) : TrackID();
    }

    float readDeviceMix (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER)
                   ? toFloat (readNumber (element[schema::deviceMix], 1.0))
                   : 1.0f;
    }

    ContainerSplit readContainerSplit (const juce::ValueTree& container)
    {
        return container.hasType (schema::CONTAINER) ? readSplitWord (container[schema::containerSplit]) : ContainerSplit::layers;
    }

    bool readContainerInstrument (const juce::ValueTree& container)
    {
        return container.hasType (schema::CONTAINER) && (bool) container[schema::containerInstrument];
    }

    float readChainVolume (const juce::ValueTree& chain)
    {
        return chain.hasType (schema::CHAIN) ? toFloat (readNumber (chain[schema::channelVolume], 0.0)) : 0.0f;
    }

    float readChainPan (const juce::ValueTree& chain)
    {
        return chain.hasType (schema::CHAIN) ? toFloat (readNumber (chain[schema::channelPan], 0.0)) : 0.0f;
    }

    std::optional<float> readChainCrossover (const juce::ValueTree& chain)
    {
        if (! chain.hasType (schema::CHAIN))
            return std::nullopt;

        const auto hertz = readValueNumber (chain[schema::chainCrossover]);
        return hertz.has_value() && std::isfinite (*hertz) ? std::optional<float> (toFloat (*hertz)) : std::nullopt;
    }

    NoteRange readChainKeys (const juce::ValueTree& chain)
    {
        if (! chain.hasType (schema::CHAIN))
            return allKeys;

        return { readWhole (chain[schema::chainLowKey], allKeys.low), readWhole (chain[schema::chainHighKey], allKeys.high) };
    }

    NoteRange readChainVelocities (const juce::ValueTree& chain)
    {
        if (! chain.hasType (schema::CHAIN))
            return allVelocities;

        return { readWhole (chain[schema::chainLowVelocity], allVelocities.low),
                 readWhole (chain[schema::chainHighVelocity], allVelocities.high) };
    }

    // What both readers read alike: whether the element is one, its ID, its place, its form and an own device's type.
    std::optional<DeviceValue> readIdentity (const juce::ValueTree& element, const DevicePlace& place)
    {
        const auto form = readDeviceForm (element);

        if (! form.has_value())
            return std::nullopt;

        DeviceValue value;
        value.form = *form;

        if (*form == Form::own)
        {
            value.type = element[schema::deviceType].toString();

            if (findInfo (value.type) == nullptr)
                return std::nullopt;
        }

        value.id = readDeviceElementID (element);
        value.place = place;
        return value;
    }

    // What both readers read alike of a form: everything but the numbers that have ranges, the values set by hand and the
    // sounds.
    void readPlainFields (const juce::ValueTree& element, DeviceValue& value)
    {
        switch (value.form)
        {
            case Form::own:
                value.off = readDeviceOff (element);
                value.sidechain = readDeviceSidechain (element);
                break;

            case Form::plugin:
                value.off = readDeviceOff (element);
                value.sidechain = readDeviceSidechain (element);
                value.plugin = readPluginIdentity (element);
                break;

            case Form::container:
                value.off = readDeviceOff (element);
                value.name = readDeviceElementName (element);
                value.split = readContainerSplit (element);
                value.instrument = readContainerInstrument (element);
                break;

            case Form::chain:
                value.name = readDeviceElementName (element);
                break;
        }
    }

    // What readHeldDevice() reads of an element but its values set by hand and its sounds: what the element's own properties
    // are as the song holds them, read without a map made (not counted: countDevicesReadAsValues()).
    std::optional<DeviceValue> readHeldFields (const juce::ValueTree& element, const DevicePlace& place)
    {
        auto value = readIdentity (element, place);

        if (! value.has_value() || ! value->id.isValid() || value->id.getRaw() >= schema::maxID)
            return std::nullopt;

        readPlainFields (element, *value);

        switch (value->form)
        {
            case Form::own:
                break;

            case Form::plugin:
            case Form::container:
                value->mix = (float) readHeldNumber (element[schema::deviceMix], schema::deviceMix, 1.0);
                break;

            case Form::chain:
            {
                value->volume = (float) readHeldNumber (element[schema::channelVolume], schema::channelVolume, 0.0);
                value->pan = (float) readHeldNumber (element[schema::channelPan], schema::channelPan, 0.0);

                value->crossover = readHeldCrossover (element[schema::chainCrossover]);
                value->keys = { readHeldWhole (element[schema::chainLowKey], schema::chainLowKey, allKeys.low),
                                readHeldWhole (element[schema::chainHighKey], schema::chainHighKey, allKeys.high) };
                value->velocities = { readHeldWhole (element[schema::chainLowVelocity], schema::chainLowVelocity, allVelocities.low),
                                      readHeldWhole (element[schema::chainHighVelocity], schema::chainHighVelocity, allVelocities.high) };
                break;
            }
        }

        return value;
    }

    // Each of an element's own properties as Phazite's writers write it (writeDeviceElement(): which, and when), given to
    // `each` as its name and its value, in the writer's order: the one list of them, which the writer and the loader's rule
    // (keepDeviceAsHeld()) both go by. Nothing allocated for a value of what the value holds (a text shares its own).
    template <typename Each>
    void forEachWrittenProperty (const DeviceValue& value, Each&& each)
    {
        const auto writeMix = [&value, &each]
        {
            if (value.mix < 1.0f)
                each (schema::deviceMix, juce::var (value.mix));
        };

        switch (value.form)
        {
            case Form::own:
            case Form::plugin:
            {
                each (schema::deviceID, juce::var ((juce::int64) value.id.getRaw()));
                each (schema::deviceType, value.isPlugin() ? getPluginType() : juce::var (value.type));

                if (value.off)
                    each (schema::deviceOff, juce::var (true));

                if (value.sidechain.isValid())
                    each (schema::deviceSidechain, juce::var ((juce::int64) value.sidechain.getRaw()));

                if (value.isPlugin())
                {
                    each (schema::pluginFormat, juce::var (value.plugin.format));
                    each (schema::pluginName, juce::var (value.plugin.name));
                    each (schema::pluginMaker, juce::var (value.plugin.maker));
                    each (schema::pluginIdentifier, juce::var (value.plugin.identifier));
                    each (schema::pluginUid, juce::var (value.plugin.uid));

                    if (value.plugin.instrument)
                        each (schema::pluginInstrument, juce::var (true));

                    writeMix();
                }

                break;
            }

            case Form::container:
            {
                each (schema::deviceID, juce::var ((juce::int64) value.id.getRaw()));
                each (schema::containerName, juce::var (value.name));

                if (value.off)
                    each (schema::deviceOff, juce::var (true));

                writeMix();

                if (const auto* word = getSplitWord (value.split))
                    each (schema::containerSplit, *word);

                if (value.instrument)
                    each (schema::containerInstrument, juce::var (true));

                break;
            }

            case Form::chain:
            {
                each (schema::chainID, juce::var ((juce::int64) value.id.getRaw()));
                each (schema::chainName, juce::var (value.name));

                if (! juce::exactlyEqual (value.volume, 0.0f))
                    each (schema::channelVolume, juce::var (value.volume));

                if (! juce::exactlyEqual (value.pan, 0.0f))
                    each (schema::channelPan, juce::var (value.pan));

                if (value.crossover.has_value())
                    each (schema::chainCrossover, juce::var (*value.crossover));

                if (value.keys.low != allKeys.low)
                    each (schema::chainLowKey, juce::var (value.keys.low));

                if (value.keys.high != allKeys.high)
                    each (schema::chainHighKey, juce::var (value.keys.high));

                if (value.velocities.low != allVelocities.low)
                    each (schema::chainLowVelocity, juce::var (value.velocities.low));

                if (value.velocities.high != allVelocities.high)
                    each (schema::chainHighVelocity, juce::var (value.velocities.high));

                break;
            }
        }
    }

    // Whether the value covers a property of an element of a form (the top of DevicesValue.h): what the writer may write of
    // it (forEachWrittenProperty()), which the loader's rule makes the element hold as the writer writes it, and nothing else.
    bool isCovered (Form form, const juce::Identifier& property)
    {
        const auto isAmong = [&property] (std::initializer_list<const juce::Identifier*> list)
        {
            for (const auto* each : list)
                if (*each == property)
                    return true;

            return false;
        };

        switch (form)
        {
            case Form::own:
                return isAmong ({ &schema::deviceID.getName(), &schema::deviceType.getName(), &schema::deviceOff.getName(),
                                  &schema::deviceSidechain.getName() });
            case Form::plugin:
                return isAmong ({ &schema::deviceID.getName(), &schema::deviceType.getName(), &schema::deviceOff.getName(),
                                  &schema::deviceSidechain.getName(), &schema::pluginFormat.getName(), &schema::pluginName.getName(),
                                  &schema::pluginMaker.getName(), &schema::pluginIdentifier.getName(), &schema::pluginUid.getName(),
                                  &schema::pluginInstrument.getName(), &schema::deviceMix.getName() });
            case Form::container:
                return isAmong ({ &schema::deviceID.getName(), &schema::deviceOff.getName(), &schema::deviceMix.getName(),
                                  &schema::containerName.getName(), &schema::containerSplit.getName(),
                                  &schema::containerInstrument.getName() });
            case Form::chain:
                return isAmong ({ &schema::chainID.getName(), &schema::chainName.getName(), &schema::channelVolume.getName(),
                                  &schema::channelPan.getName(), &schema::chainCrossover.getName(), &schema::chainLowKey.getName(),
                                  &schema::chainHighKey.getName(), &schema::chainLowVelocity.getName(),
                                  &schema::chainHighVelocity.getName() });
        }

        return false;
    }

    // Whether an element's own properties the value covers are those the writer writes of what it holds (held), each as the
    // same text (SongText.h: isSameValue()), and no other: what the loader's rule leaves as it is. Nothing allocated.
    bool holdsPropertiesAsWritten (const juce::ValueTree& element, const DeviceValue& held)
    {
        auto written = 0;
        auto same = true;

        forEachWrittenProperty (held,
                                [&element, &written, &same] (const juce::Identifier& property, const juce::var& value)
                                {
                                    const auto* there = element.getPropertyPointer (property);
                                    same = same && there != nullptr && isSameValue (*there, value);
                                    ++written;
                                });

        if (! same)
            return false;

        auto covered = 0;

        for (int i = 0; i < element.getNumProperties(); ++i)
            if (isCovered (held.form, element.getPropertyName (i)))
                ++covered;

        return covered == written;
    }

    // The element's own properties the value covers made those the writer writes of what it holds: each set where it is
    // another text, each the writer leaves out taken away; the others left as they are. Whether any changed.
    bool keepPropertiesAsWritten (juce::ValueTree& element, const DeviceValue& held)
    {
        auto changed = false;
        std::vector<juce::Identifier> written;

        forEachWrittenProperty (held,
                                [&element, &written, &changed] (const juce::Identifier& property, const juce::var& value)
                                {
                                    written.push_back (property);

                                    if (const auto* there = element.getPropertyPointer (property);
                                        there == nullptr || ! isSameValue (*there, value))
                                    {
                                        element.setProperty (property, value, nullptr);
                                        changed = true;
                                    }
                                });

        for (auto i = element.getNumProperties(); --i >= 0;)
        {
            const auto property = element.getPropertyName (i);

            if (isCovered (held.form, property) && std::find (written.begin(), written.end(), property) == written.end())
            {
                element.removeProperty (property, nullptr);
                changed = true;
            }
        }

        return changed;
    }

    // Whether one of Phazite's own devices' VALUES holds what the song holds of them as the writer writes it: every property
    // a value of a parameter its type has, as the song holds it (SavedValues.h: readHeldSavedValue(): none at its default,
    // within its range...), written as the writer writes it (a choice by its name, any other by its number, compared as
    // text), at least one, and nothing else. Nothing allocated: a choice's name compared as it is, not made a text.
    bool holdsValuesAsWritten (const juce::ValueTree& values, const devices::DeviceInfo& info)
    {
        if (values.getNumProperties() == 0 || values.getNumChildren() > 0)
            return false;

        for (int i = 0; i < values.getNumProperties(); ++i)
        {
            const auto name = values.getPropertyName (i);
            const auto index = devices::findParameter (info, std::string_view (name.getCharPointer().getAddress()));

            if (index < 0)
                return false;

            const auto& parameter = info.parameters[(size_t) index];
            const auto& saved = values[name];
            const auto held = readHeldSavedValue (parameter, saved);

            if (! held.has_value())
                return false;

            if (parameter.unit == devices::Unit::choice)
            {
                if (const auto choice = devices::getChoiceName (parameter, juce::roundToInt (*held - parameter.minimum)); ! choice.empty())
                {
                    if (! saved.isString() || std::string_view (saved.toString().toRawUTF8()) != choice)
                        return false;

                    continue;
                }
            }

            if (! isSameValue (saved, juce::var (*held)))
                return false;
        }

        return true;
    }

    // Whether one of Phazite's own devices' SOUNDS holds sounds as the writer writes them: each of a property its type has,
    // none empty, at least one, and nothing else. Nothing allocated.
    bool holdsSoundsAsWritten (const juce::ValueTree& sounds, std::string_view typeID)
    {
        const auto* table = findSoundTable (typeID);

        if (table == nullptr || sounds.getNumProperties() == 0 || sounds.getNumChildren() > 0)
            return false;

        for (int i = 0; i < sounds.getNumProperties(); ++i)
        {
            const auto name = sounds.getPropertyName (i);

            if (table->indices.find (name.getCharPointer().getAddress()) == table->indices.end() || sounds[name].toString().isEmpty())
                return false;
        }

        return true;
    }

    // Whether an element holds what another does, written as text: the same type, the same properties, each written the
    // same (SongText.h: isSameValue()), and the same children in the same order, each so.
    bool isWrittenAs (const juce::ValueTree& element, const juce::ValueTree& written)
    {
        if (element.getType() != written.getType() || element.getNumProperties() != written.getNumProperties()
            || element.getNumChildren() != written.getNumChildren())
            return false;

        for (int i = 0; i < written.getNumProperties(); ++i)
        {
            const auto name = written.getPropertyName (i);
            const auto* held = element.getPropertyPointer (name);

            if (held == nullptr || ! isSameValue (*held, written[name]))
                return false;
        }

        for (int i = 0; i < written.getNumChildren(); ++i)
            if (! isWrittenAs (element.getChild (i), written.getChild (i)))
                return false;

        return true;
    }

    // Each TRACK of a song's tree in the track list, a folder before the tracks in it, then the MASTER.
    void forEachChannelElement (const juce::ValueTree& song, const std::function<void (const juce::ValueTree&)>& each)
    {
        const std::function<void (const juce::ValueTree&)> readTracks = [&each, &readTracks] (const juce::ValueTree& parent)
        {
            for (const auto& child : parent)
            {
                if (child.hasType (schema::TRACK))
                {
                    each (child);

                    if (child[schema::trackKind].toString() == schema::trackIsFolder)
                        readTracks (child);
                }
            }
        };

        readTracks (song.getChildWithName (schema::TRACKS));

        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
            each (master);
    }
} // namespace

//==============================================================================
bool DeviceValue::isInstrument() const
{
    switch (form)
    {
        case Form::own:
        {
            const auto* info = findInfo (type);
            return info != nullptr && info->kind == devices::DeviceKind::instrument;
        }

        case Form::plugin:
            return plugin.instrument;
        case Form::container:
            return instrument;
        case Form::chain:
            break;
    }

    return false;
}

float DeviceValue::getParameterValue (int index) const
{
    const auto* info = form == Form::own ? findInfo (type) : nullptr;

    if (info == nullptr || ! juce::isPositiveAndBelow (index, (int) info->parameters.size()))
        return 0.0f;

    const auto* held = values.find ((std::uint32_t) index);
    return held != nullptr ? *held : info->parameters[(size_t) index].defaultValue;
}

bool DeviceValue::isSameAs (const DeviceValue& other) const noexcept
{
    return id == other.id && place == other.place && form == other.form && off == other.off && sidechain == other.sidechain
           && type == other.type && values.identity() == other.values.identity() && sounds.identity() == other.sounds.identity()
           && plugin == other.plugin && juce::exactlyEqual (mix, other.mix) && name == other.name && split == other.split
           && instrument == other.instrument && juce::exactlyEqual (volume, other.volume) && juce::exactlyEqual (pan, other.pan)
           && crossover.has_value() == other.crossover.has_value()
           && (! crossover.has_value() || juce::exactlyEqual (*crossover, *other.crossover)) && keys == other.keys
           && velocities == other.velocities;
}

bool DeviceValue::operator== (const DeviceValue& other) const
{
    return id == other.id && place == other.place && form == other.form && off == other.off && sidechain == other.sidechain
           && type == other.type && plugin == other.plugin && juce::exactlyEqual (mix, other.mix) && name == other.name
           && split == other.split && instrument == other.instrument && juce::exactlyEqual (volume, other.volume)
           && juce::exactlyEqual (pan, other.pan) && crossover.has_value() == other.crossover.has_value()
           && (! crossover.has_value() || juce::exactlyEqual (*crossover, *other.crossover)) && keys == other.keys
           && velocities == other.velocities && holdSameEntries (values, other.values) && holdSameEntries (sounds, other.sounds);
}

const immer::flex_vector<DeviceID>* SongDevices::findList (const DevicePlace& place) const
{
    switch (getListKind (place))
    {
        case 2:
            return chainDevices.find (place.chain.getRaw());
        case 1:
            return chains.find (place.container.getRaw());
        default:
            break;
    }

    return channels.find (getChannelKey (place));
}

bool SongDevices::isSameAs (const SongDevices& other) const noexcept
{
    return devices.identity() == other.devices.identity() && channels.identity() == other.channels.identity()
           && chains.identity() == other.chains.identity() && chainDevices.identity() == other.chainDevices.identity();
}

bool SongDevices::operator== (const SongDevices& other) const
{
    return devices == other.devices && channels == other.channels && chains == other.chains && chainDevices == other.chainDevices;
}

namespace
{
    // What a container or a chain holds given the place it is at: a container's chains its channel, each chain's devices
    // their chain's channel and container; a chain's devices its channel and container. What withDevice() keeps in step.
    SongDevices withPlacesBelow (SongDevices devices, const DeviceValue& owner)
    {
        const auto placeAll = [&devices] (const IDList& list, const DevicePlace& place)
        {
            for (const auto inside : list)
            {
                if (const auto* found = devices.find (inside); found != nullptr && found->place != place)
                {
                    auto moved = *found;
                    moved.place = place;
                    devices.devices = devices.devices.set (inside.getRaw(), std::move (moved));
                }
            }
        };

        if (owner.form == Form::chain)
        {
            if (const auto* list = devices.chainDevices.find (owner.id.getRaw()))
                placeAll (IDList (*list), { owner.place.track, owner.place.onMaster, owner.place.container, owner.id });
        }
        else if (owner.form == Form::container)
        {
            if (const auto* list = devices.chains.find (owner.id.getRaw()))
            {
                const auto chains = *list;
                placeAll (chains, { owner.place.track, owner.place.onMaster, owner.id, {} });

                for (const auto chain : chains)
                    if (const auto* inside = devices.chainDevices.find (chain.getRaw()))
                        placeAll (IDList (*inside), { owner.place.track, owner.place.onMaster, owner.id, chain });
            }
        }

        return devices;
    }
} // namespace

SongDevices withDevice (const SongDevices& devices, DeviceValue value, int index)
{
    const auto id = value.id;
    const auto place = value.place;
    const auto* old = devices.find (id);
    const auto moves = old != nullptr && old->place != place;
    auto next = devices;
    next.devices = next.devices.set (id.getRaw(), value);

    // What it holds goes with it.
    if (moves)
        next = withPlacesBelow (std::move (next), value);

    // One whose list stays keeps its place in it.
    if (old != nullptr && isSameList (old->place, place))
        return next;

    if (old != nullptr)
        next = withoutInList (std::move (next), old->place, id);

    return withInList (std::move (next), place, id, index);
}

SongDevices withoutDevice (const SongDevices& devices, DeviceID id)
{
    const auto* old = devices.find (id);

    if (old == nullptr)
        return devices;

    auto next = devices;

    // What it holds first: a container's chains (each with its devices), a chain's devices.
    if (old->form == Form::container)
    {
        if (const auto* chains = next.chains.find (id.getRaw()))
            for (const auto chain : IDList (*chains))
                next = withoutDevice (next, chain);

        next.chains = next.chains.erase (id.getRaw());
    }
    else if (old->form == Form::chain)
    {
        if (const auto* inside = next.chainDevices.find (id.getRaw()))
            for (const auto device : *inside)
                next.devices = next.devices.erase (device.getRaw());

        next.chainDevices = next.chainDevices.erase (id.getRaw());
    }

    next = withoutInList (std::move (next), old->place, id);
    next.devices = next.devices.erase (id.getRaw());
    return next;
}

SongDevices withListOrder (const SongDevices& devices, const DevicePlace& owner, immer::flex_vector<DeviceID> order)
{
    const auto* list = devices.findList (owner);

    if (list == nullptr || list->size() != order.size())
        return devices;

    std::set<DeviceID> held (list->begin(), list->end()), given (order.begin(), order.end());

    if (held != given || given.size() != order.size())
        return devices;

    auto next = devices;
    auto& lists = getLists (next, owner);
    lists = lists.set (getListKey (owner), std::move (order));
    return next;
}

bool holdsItsLists (const SongDevices& devices)
{
    const auto isOnChannel = [] (const DevicePlace& place) { return place.onMaster != place.track.isValid(); };
    const auto sameChannel = [] (const DevicePlace& a, const DevicePlace& b) { return a.onMaster == b.onMaster && a.track == b.track; };

    for (const auto& [key, value] : devices.devices)
    {
        if (key != value.id.getRaw() || ! value.id.isValid() || ! isOnChannel (value.place))
            return false;

        switch (value.form)
        {
            case Form::own:
            case Form::plugin:
            {
                if (! value.place.chain.isValid())
                {
                    if (value.place.container.isValid())
                        return false;

                    break;
                }

                const auto* chain = devices.find (value.place.chain);

                if (chain == nullptr || chain->form != Form::chain || chain->place.container != value.place.container
                    || ! sameChannel (chain->place, value.place))
                    return false;

                break;
            }

            case Form::container:
                if (value.place.container.isValid() || value.place.chain.isValid())
                    return false;

                break;

            case Form::chain:
            {
                const auto* container = value.place.chain.isValid() ? nullptr : devices.find (value.place.container);

                if (container == nullptr || container->form != Form::container || ! sameChannel (container->place, value.place))
                    return false;

                break;
            }
        }
    }

    std::set<std::uint64_t> listed;

    const auto holdsLists = [&devices, &listed] (const Lists& lists, int kind)
    {
        for (const auto& [key, list] : lists)
        {
            if (list.empty())
                return false;

            if (kind > 0)
            {
                const auto* owner = devices.devices.find (key);

                if (owner == nullptr || owner->form != (kind == 1 ? Form::container : Form::chain))
                    return false;
            }

            for (const auto& id : list)
            {
                const auto* value = devices.find (id);

                if (value == nullptr || getListKind (value->place) != kind || getListKey (value->place) != key
                    || ! listed.insert (id.getRaw()).second)
                    return false;
            }
        }

        return true;
    };

    return holdsLists (devices.channels, 0) && holdsLists (devices.chains, 1) && holdsLists (devices.chainDevices, 2)
           && listed.size() == devices.devices.size();
}

//==============================================================================
DevicePlace readDevicePlace (const juce::ValueTree& element)
{
    DevicePlace place;
    auto parent = element.getParent();

    if (element.hasType (schema::DEVICE) && parent.hasType (schema::CHAIN))
    {
        place.chain = readDeviceElementID (parent);
        parent = parent.getParent();
    }

    if (parent.hasType (schema::CONTAINER) && (element.hasType (schema::CHAIN) || place.chain.isValid()))
    {
        place.container = readDeviceElementID (parent);
        parent = parent.getParent();
    }

    if (parent.hasType (schema::MASTER))
        place.onMaster = true;
    else if (parent.hasType (schema::TRACK))
        place.track = TrackID::fromRaw (readIDOf (parent[schema::trackID]));

    return place;
}

std::optional<DeviceValue::Form> readDeviceForm (const juce::ValueTree& element)
{
    if (element.hasType (schema::DEVICE))
        return element[schema::deviceType].toString() == schema::deviceIsPlugin ? Form::plugin : Form::own;

    if (element.hasType (schema::CONTAINER))
        return Form::container;

    if (element.hasType (schema::CHAIN))
        return Form::chain;

    return std::nullopt;
}

std::optional<DeviceValue> readDeviceValue (const juce::ValueTree& element, const DevicePlace& place)
{
    auto value = readIdentity (element, place);

    if (! value.has_value())
        return std::nullopt;

    readPlainFields (element, *value);

    switch (value->form)
    {
        case Form::own:
            value->values = readValues (element, *findInfo (value->type), [] (const devices::ParameterInfo& info, const juce::var& saved)
                                        { return std::optional<float> (readSavedValue (info, saved)); });
            value->sounds = readSounds (element, std::string_view (value->type.toRawUTF8()));
            break;

        case Form::plugin:
        case Form::container:
            value->mix = readDeviceMix (element);
            break;

        case Form::chain:
            value->volume = readChainVolume (element);
            value->pan = readChainPan (element);
            value->crossover = readChainCrossover (element);
            value->keys = readChainKeys (element);
            value->velocities = readChainVelocities (element);
            break;
    }

    devicesRead.fetch_add (1, std::memory_order_relaxed);
    return value;
}

std::optional<DeviceValue> readHeldDevice (const juce::ValueTree& element, const DevicePlace& place)
{
    auto value = readHeldFields (element, place);

    if (! value.has_value())
        return std::nullopt;

    if (value->form == Form::own)
    {
        value->values = readValues (element, *findInfo (value->type), readHeldSavedValue);
        value->sounds = readSounds (element, std::string_view (value->type.toRawUTF8()));
    }

    devicesRead.fetch_add (1, std::memory_order_relaxed);
    return value;
}

std::optional<DeviceValue> readHeldDeviceFields (const juce::ValueTree& element, const DevicePlace& place)
{
    return readHeldFields (element, place);
}

juce::ValueTree writeDeviceElement (const DeviceValue& value)
{
    juce::ValueTree element (value.form == Form::container ? schema::CONTAINER
                             : value.form == Form::chain   ? schema::CHAIN
                                                           : schema::DEVICE);

    forEachWrittenProperty (value, [&element] (const juce::Identifier& property, const juce::var& written)
                            { element.setProperty (property, written, nullptr); });

    if (value.form != Form::own)
        return element;

    // Its SOUNDS, then its VALUES, as the song's files hold them.
    if (const auto* table = findSoundTable (std::string_view (value.type.toRawUTF8())); table != nullptr && ! value.sounds.empty())
    {
        juce::ValueTree sounds (schema::SOUNDS);

        for (size_t i = 0; i < table->properties.size(); ++i)
            if (const auto* sound = value.sounds.find ((std::uint16_t) i))
                sounds.setProperty (table->properties[i], *sound, nullptr);

        if (sounds.getNumProperties() > 0)
            element.appendChild (sounds, nullptr);
    }

    if (const auto* info = findInfo (value.type); info != nullptr && ! value.values.empty())
    {
        juce::ValueTree values (schema::VALUES);

        for (size_t i = 0; i < info->parameters.size(); ++i)
            if (const auto* held = value.values.find ((std::uint32_t) i))
                values.setProperty (juce::Identifier (toString (info->parameters[i].id)), toSavedValue (info->parameters[i], *held),
                                    nullptr);

        if (values.getNumProperties() > 0)
            element.appendChild (values, nullptr);
    }

    return element;
}

bool keepDeviceAsHeld (juce::ValueTree element)
{
    // Its own properties first, read without its values set by hand or its sounds (no map made), and rewritten only where
    // one isn't as the writer writes it.
    const auto fields = readHeldFields (element, {});

    if (! fields.has_value())
        return false;

    auto changed = ! holdsPropertiesAsWritten (element, *fields) && keepPropertiesAsWritten (element, *fields);

    // A plug-in plays no sounds of Phazite's.
    if (fields->form == Form::plugin)
    {
        for (auto sounds = element.getChildWithName (schema::SOUNDS); sounds.isValid(); sounds = element.getChildWithName (schema::SOUNDS))
        {
            element.removeChild (sounds, nullptr);
            changed = true;
        }
    }

    if (fields->form != Form::own)
        return changed;

    // Its VALUES and its SOUNDS, looked through without a value made, read whole and written again only where one isn't as
    // the writer writes it: the loader goes through every device, a full drum sampler's thousands of entries among them.
    const auto values = element.getChildWithName (schema::VALUES);
    const auto sounds = element.getChildWithName (schema::SOUNDS);

    const auto needsWriting = (values.isValid() && ! holdsValuesAsWritten (values, *findInfo (fields->type)))
                              || (sounds.isValid() && ! holdsSoundsAsWritten (sounds, std::string_view (fields->type.toRawUTF8())));

    // Read whole as readHeldFields() read it: one.
    if (const auto held = needsWriting ? readHeldDevice (element, readDevicePlace (element)) : std::nullopt; held.has_value())
    {
        const auto written = writeDeviceElement (*held);

        for (const auto* type : { &schema::SOUNDS, &schema::VALUES })
        {
            auto child = element.getChildWithName (*type);
            const auto kept = written.getChildWithName (*type);

            if (! child.isValid() || isWrittenAs (child, kept))
                continue;

            if (kept.isValid())
                child.copyPropertiesAndChildrenFrom (kept, nullptr);
            else
                element.removeChild (child, nullptr);

            changed = true;
        }
    }

    // Its VALUES and its SOUNDS stay in the order they are in: Phazite's writers write either first (a drum kit's pads'
    // values before their sounds, a sampler's sound before its values), and a song they wrote opens as it was written.
    return changed;
}

namespace
{
    // Every device, container and chain of a song's tree, each read by the reader given.
    SongDevices readTreeDevices (const juce::ValueTree& song,
                                 const std::function<std::optional<DeviceValue> (const juce::ValueTree&, const DevicePlace&)>& read)
    {
        SongDevices devices;

        // Each read and added after the others of its list; one of an ID read already, or none, with what it holds left out.
        const auto add = [&devices, &read] (const juce::ValueTree& element)
        {
            auto value = read (element, readDevicePlace (element));

            if (! value.has_value() || devices.find (value->id) != nullptr)
                return false;

            devices = withDevice (devices, std::move (*value));
            return true;
        };

        forEachChannelElement (song,
                               [&add] (const juce::ValueTree& channel)
                               {
                                   for (const auto& child : channel)
                                   {
                                       if (! (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)) || ! add (child)
                                           || ! child.hasType (schema::CONTAINER))
                                           continue;

                                       for (const auto& chain : child)
                                           if (chain.hasType (schema::CHAIN) && add (chain))
                                               for (const auto& device : chain)
                                                   if (device.hasType (schema::DEVICE))
                                                       add (device);
                                   }
                               });

        return devices;
    }
} // namespace

SongDevices readDevicesValue (const juce::ValueTree& song)
{
    return readTreeDevices (song,
                            [] (const juce::ValueTree& element, const DevicePlace& place) { return readDeviceValue (element, place); });
}

SongDevices readDevicesFields (const juce::ValueTree& song)
{
    return readTreeDevices (song, [] (const juce::ValueTree& element, const DevicePlace& place)
                            { return readHeldDeviceFields (element, place); });
}

bool setDeviceProperty (DeviceValue& value, const juce::Identifier& element, const juce::Identifier& property, const juce::var& held)
{
    const auto isDevice = value.form == Form::own || value.form == Form::plugin;
    const auto* info = value.form == Form::own ? findInfo (value.type) : nullptr;

    if (element == schema::VALUES)
    {
        const auto index = info != nullptr ? devices::findParameter (*info, std::string_view (property.getCharPointer().getAddress())) : -1;

        if (index < 0)
            return false;

        const auto kept = readHeldSavedValue (info->parameters[(size_t) index], held);
        value.values = kept.has_value() ? value.values.set ((std::uint32_t) index, *kept) : value.values.erase ((std::uint32_t) index);
        return true;
    }

    if (element == schema::SOUNDS)
    {
        const auto index = info != nullptr ? findSoundIndex (std::string_view (value.type.toRawUTF8()), property) : -1;

        if (index < 0)
            return false;

        const auto sound = held.toString();
        value.sounds = sound.isNotEmpty() ? value.sounds.set ((std::uint16_t) index, sound) : value.sounds.erase ((std::uint16_t) index);
        return true;
    }

    if ((element == schema::DEVICE && ! isDevice) || (element == schema::CONTAINER && value.form != Form::container)
        || (element == schema::CHAIN && value.form != Form::chain)
        || ! (element == schema::DEVICE || element == schema::CONTAINER || element == schema::CHAIN))
        return false;

    // What every form has: its ID ("id": deviceID, chainID).
    if (property == schema::deviceID)
    {
        value.id = DeviceID::fromRaw (readID (held));
        return true;
    }

    if (value.form == Form::chain)
    {
        if (property == schema::chainName)
            value.name = held.toString();
        else if (property == schema::channelVolume)
            value.volume = (float) readHeldNumber (held, schema::channelVolume, 0.0);
        else if (property == schema::channelPan)
            value.pan = (float) readHeldNumber (held, schema::channelPan, 0.0);
        else if (property == schema::chainCrossover)
            value.crossover = readHeldCrossover (held);
        else if (property == schema::chainLowKey)
            value.keys.low = readHeldWhole (held, schema::chainLowKey, allKeys.low);
        else if (property == schema::chainHighKey)
            value.keys.high = readHeldWhole (held, schema::chainHighKey, allKeys.high);
        else if (property == schema::chainLowVelocity)
            value.velocities.low = readHeldWhole (held, schema::chainLowVelocity, allVelocities.low);
        else if (property == schema::chainHighVelocity)
            value.velocities.high = readHeldWhole (held, schema::chainHighVelocity, allVelocities.high);
        else
            return false;

        return true;
    }

    // A device's and a container's.
    if (property == schema::deviceOff)
    {
        value.off = (bool) held;
        return true;
    }

    if (property == schema::deviceMix && value.form != Form::own)
    {
        value.mix = (float) readHeldNumber (held, schema::deviceMix, 1.0);
        return true;
    }

    if (value.form == Form::container)
    {
        if (property == schema::containerName)
            value.name = held.toString();
        else if (property == schema::containerSplit)
            value.split = readSplitWord (held);
        else if (property == schema::containerInstrument)
            value.instrument = (bool) held;
        else
            return false;

        return true;
    }

    if (property == schema::deviceSidechain)
    {
        value.sidechain = TrackID::fromRaw (readID (held));
        return true;
    }

    if (property == schema::deviceType)
    {
        const auto type = held.toString();

        if (value.isPlugin() || type == schema::deviceIsPlugin)
            return value.isPlugin() && type == schema::deviceIsPlugin;

        const auto* now = findInfo (type);

        if (now == nullptr || info == nullptr)
            return false;

        // The values and the sounds of the names the new type has, read by its parameters and properties of those names.
        if (type != value.type)
        {
            const auto before = std::string_view (value.type.toRawUTF8()), after = std::string_view (type.toRawUTF8());
            const auto& beforeSounds = getSoundPropertiesOf (before);
            immer::map<std::uint32_t, float> values;
            immer::map<std::uint16_t, juce::String> sounds;

            for (const auto& [index, was] : value.values)
            {
                if (index >= info->parameters.size())
                    continue;

                const auto& parameter = info->parameters[index];

                if (const auto at = devices::findParameter (*now, parameter.id); at >= 0)
                    if (const auto read = readHeldSavedValue (now->parameters[(size_t) at], toSavedValue (parameter, was)))
                        values = values.set ((std::uint32_t) at, *read);
            }

            for (const auto& [index, sound] : value.sounds)
                if (index < beforeSounds.size())
                    if (const auto at = findSoundIndex (after, beforeSounds[index]); at >= 0)
                        sounds = sounds.set ((std::uint16_t) at, sound);

            value.values = values;
            value.sounds = sounds;
            value.type = type;
        }

        return true;
    }

    if (value.isPlugin())
    {
        if (property == schema::pluginFormat)
            value.plugin.format = held.toString();
        else if (property == schema::pluginName)
            value.plugin.name = held.toString();
        else if (property == schema::pluginMaker)
            value.plugin.maker = held.toString();
        else if (property == schema::pluginIdentifier)
            value.plugin.identifier = held.toString();
        else if (property == schema::pluginUid)
            value.plugin.uid = readWhole (held, 0);
        else if (property == schema::pluginInstrument)
            value.plugin.instrument = (bool) held;
        else
            return false;

        return true;
    }

    return false;
}

//==============================================================================
DeviceID readDeviceElementID (const juce::ValueTree& element)
{
    if (element.hasType (schema::CHAIN))
        return DeviceID::fromRaw (readIDOf (element[schema::chainID]));

    return element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER)
               ? DeviceID::fromRaw (readIDOf (element[schema::deviceID]))
               : DeviceID();
}

juce::String readDeviceType (const juce::ValueTree& device)
{
    return device.hasType (schema::DEVICE) ? device[schema::deviceType].toString() : juce::String();
}

PluginIdentity readPluginIdentity (const juce::ValueTree& device)
{
    if (! device.hasType (schema::DEVICE))
        return {};

    return { .format = device[schema::pluginFormat].toString(),
             .name = device[schema::pluginName].toString(),
             .maker = device[schema::pluginMaker].toString(),
             .identifier = device[schema::pluginIdentifier].toString(),
             .uid = readWhole (device[schema::pluginUid], 0),
             .instrument = (bool) device[schema::pluginInstrument] };
}

bool readIsInstrument (const juce::ValueTree& element)
{
    switch (readDeviceForm (element).value_or (Form::chain))
    {
        case Form::own:
        {
            const auto* info = findInfo (element[schema::deviceType].toString());
            return info != nullptr && info->kind == devices::DeviceKind::instrument;
        }

        case Form::plugin:
            return element[schema::pluginInstrument];
        case Form::container:
            return element[schema::containerInstrument];
        case Form::chain:
            break;
    }

    return false;
}

juce::String readDeviceElementName (const juce::ValueTree& element)
{
    if (element.hasType (schema::CONTAINER))
        return element[schema::containerName].toString();

    return element.hasType (schema::CHAIN) ? element[schema::chainName].toString() : juce::String();
}

float readDeviceParameterValue (const juce::ValueTree& device, const devices::ParameterInfo& info)
{
    return readDeviceParameterValue (device, info, juce::Identifier (toString (info.id)));
}

float readDeviceParameterValue (const juce::ValueTree& device, const devices::ParameterInfo& info, const juce::Identifier& id)
{
    return readSavedValue (info, device.getChildWithName (schema::VALUES).getProperty (id));
}

juce::String readSoundsProperty (const juce::ValueTree& sounds, const juce::Identifier& property)
{
    return sounds.hasType (schema::SOUNDS) ? sounds[property].toString() : juce::String();
}

std::vector<DeviceID> readDeviceList (const juce::ValueTree& owner)
{
    std::vector<DeviceID> ids;
    const auto isListed = [&owner] (const juce::ValueTree& child)
    {
        if (owner.hasType (schema::TRACK) || owner.hasType (schema::MASTER))
            return child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER);

        if (owner.hasType (schema::CONTAINER))
            return child.hasType (schema::CHAIN);

        return owner.hasType (schema::CHAIN) && child.hasType (schema::DEVICE);
    };

    for (const auto& child : owner)
        if (isListed (child))
            ids.push_back (readDeviceElementID (child));

    return ids;
}

bool holdsDevice (const Song& song, DeviceID id, bool chainsToo)
{
    if (! id.isValid())
        return false;

    // A look in the devices' area.
    const auto* held = song.getValues()->devices.get().find (id);
    return held != nullptr && (chainsToo || held->form != Form::chain);
}

//==============================================================================
const std::vector<juce::Identifier>& getSoundPropertiesOf (std::string_view typeID)
{
    static const std::vector<juce::Identifier> none;
    const auto* table = findSoundTable (typeID);
    return table != nullptr ? table->properties : none;
}

int findSoundIndex (std::string_view typeID, const juce::Identifier& property)
{
    const auto* table = findSoundTable (typeID);

    if (table == nullptr)
        return -1;

    const auto found = table->indices.find (property.getCharPointer().getAddress());
    return found != table->indices.end() ? found->second : -1;
}

juce::String getDeviceSound (const DeviceValue& value, const juce::Identifier& property)
{
    if (value.form != Form::own)
        return {};

    const auto index = findSoundIndex (std::string_view (value.type.toRawUTF8()), property);
    const auto* sound = index >= 0 ? value.sounds.find ((std::uint16_t) index) : nullptr;
    return sound != nullptr ? *sound : juce::String();
}

//==============================================================================
std::uint64_t findLargestIDIn (const SongDevices& devices)
{
    std::uint64_t largest = 0;

    for (const auto& [id, value] : devices.devices)
        largest = countHeldID (largest, id);

    return largest;
}

void forEachIDHeld (const SongDevices& devices, const std::function<void (std::uint64_t)>& each)
{
    for (const auto& [id, value] : devices.devices)
        each (id);
}

void forEachIDNamed (const SongDevices& devices, const std::function<void (std::uint64_t)>& each)
{
    for (const auto& [id, value] : devices.devices)
        if (value.sidechain.isValid())
            each (value.sidechain.getRaw());
}

std::uint64_t countDevicesReadAsValues() noexcept
{
    return devicesRead.load (std::memory_order_relaxed);
}

std::uint64_t countDeviceEntriesCompared() noexcept
{
    return entriesCompared.load (std::memory_order_relaxed);
}
} // namespace phazite
