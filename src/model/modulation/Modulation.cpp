// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Modulation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Numbers.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Whether a channel's element can hold modulators: a track's, a folder's or the master's; and macros: a track's or a
    // return's, which hold clips or take them in, not a folder's or the master's.
    bool canHoldModulators (const juce::ValueTree& channel)
    {
        return channel.hasType (schema::TRACK) || channel.hasType (schema::MASTER);
    }

    bool canHoldMacros (const juce::ValueTree& owner)
    {
        return (owner.hasType (schema::TRACK) && ! isFolderState (owner)) || owner.hasType (schema::CONTAINER);
    }

    // Whether two places have one owner (ModulationValue.h: ModulatorPlace): the same container where either names one,
    // else the same channel.
    bool isSameOwner (const ModulatorPlace& a, const ModulatorPlace& b)
    {
        if (a.container.isValid() || b.container.isValid())
            return a.container == b.container;

        return a.onMaster == b.onMaster && (a.onMaster || a.track == b.track);
    }

    // Whether a place is on a channel: its own or a container's on it.
    bool isOnChannel (const ModulatorPlace& place, const ModulatorPlace& channel)
    {
        return place.onMaster == channel.onMaster && (place.onMaster || place.track == channel.track);
    }

    // An amount as Phazite writes one: to a ten-thousandth.
    double writeAmount (float amount)
    {
        return std::round ((double) juce::jlimit (-1.0f, 1.0f, amount) * 1.0e4) / 1.0e4;
    }

    std::atomic<std::uint64_t> looks { 0 }; // countModulationLooks(): a count alone, which orders nothing

    // The song's modulation now: its area's value; for a song that holds no values (a check's copy, which holds everything in
    // its tree: tests/SongCheck.cpp), read from its tree. A copy, which shares what it holds.
    SongModulation getArea (const Song& song)
    {
        looks.fetch_add (1, std::memory_order_relaxed);
        const auto* values = song.getValues();
        return values != nullptr ? values->modulation.get() : readModulationValue (song.getState());
    }

    //==============================================================================
    // A channel's own place: the master's, or a track's.
    ModulatorPlace getChannelPlace (const Song& song, TrackID track)
    {
        ModulatorPlace place;

        if (findChainState (song, track).hasType (schema::MASTER))
            place.onMaster = true;
        else
            place.track = track;

        return place;
    }

    // An element's step in a merge's clash's path (model/versions/SongMerge.cpp): its type and key ("MODULATOR:93").
    juce::String getPathStep (const juce::ValueTree& element)
    {
        return element.getType().toString() + ":" + schema::getElementKey (element);
    }

    // The place an owner's element in the song gives what it holds: a track's or the master's own, or a container's on one;
    // none for another element.
    std::optional<ModulatorPlace> readOwnerPlace (const juce::ValueTree& owner)
    {
        if (owner.hasType (schema::MASTER))
            return ModulatorPlace { .onMaster = true };

        if (owner.hasType (schema::TRACK))
            return ModulatorPlace { .track = TrackID::fromRaw (readID (owner[schema::trackID])) };

        if (! owner.hasType (schema::CONTAINER))
            return std::nullopt;

        auto place = readOwnerPlace (owner.getParent());

        if (! place.has_value() || place->container.isValid())
            return std::nullopt;

        place->container = DeviceID::fromRaw (readID (owner[schema::deviceID]));
        return place;
    }

    // Each owner of modulators and macros the song has, in the song's order: each channel (its tracks', in their order, then
    // the master's), its own and then its containers', whether or not it holds any.
    void forEachOwner (const Song& song, const std::function<void (const ModulatorPlace&)>& each)
    {
        auto channels = getTrackStates (song);
        channels.push_back (findChainState (song, {}));

        for (const auto& channel : channels)
        {
            const auto place =
                channel.hasType (schema::MASTER) ? ModulatorPlace { .onMaster = true } : ModulatorPlace { .track = getTrackID (channel) };
            each (place);

            // Its containers, by their IDs (Mixer.h: getDeviceOutline()).
            for (const auto& device : getDeviceOutline (song, place.onMaster ? TrackID() : place.track))
                if (device.form == DeviceValue::Form::container && ! device.container.isValid())
                    each ({ .track = place.track, .onMaster = place.onMaster, .container = device.id });
        }
    }

    // Every modulator and macro of the song, in the song's order (forEachOwner()), each owner's in its order.
    std::vector<ModulatorValue> getEveryModulator (const Song& song)
    {
        std::vector<ModulatorValue> every;
        forEachOwner (song,
                      [&song, &every] (const ModulatorPlace& owner)
                      {
                          for (auto& value : getOwnedModulators (song, owner))
                              every.push_back (std::move (value));
                      });
        return every;
    }
} // namespace

//==============================================================================
SongModulation getModulationValue (const Song& song)
{
    return getArea (song);
}

std::optional<ModulatorValue> getModulatorValue (const Song& song, ModulatorID id)
{
    // A look in the area; for a song without values (one the loader makes consistent, a check's copy), its one element of
    // the ID read from its tree, the others not (the loader's checks ask for each curve's and each link's parameter).
    looks.fetch_add (1, std::memory_order_relaxed);

    if (const auto* values = song.getValues())
    {
        const auto* value = values->modulation.get().find (id);
        return value != nullptr ? std::optional<ModulatorValue> (*value) : std::nullopt;
    }

    return readModulatorValueIn (song.getState(), id);
}

std::uint64_t countModulationLooks() noexcept
{
    return looks.load (std::memory_order_relaxed);
}

std::vector<ModulatorValue> getOwnedModulators (const Song& song, const ModulatorPlace& owner)
{
    std::vector<ModulatorValue> modulators;
    const auto modulation = getArea (song);

    if (const auto* list = modulation.findList (owner))
        for (const auto id : *list)
            if (const auto* value = modulation.find (id))
                modulators.push_back (*value);

    return modulators;
}

std::vector<Modulation> findModulationsOf (const Song& song, const ParameterRef& parameter)
{
    // A look at each modulator's modulations, none read from elements; those found put in the song's order only where there
    // are several (a knob's ring has mostly none or one).
    std::vector<Modulation> modulations;
    const auto modulation = getArea (song);

    for (const auto& [id, value] : modulation.modulators)
        if (const auto* entry = value.findModulation (parameter))
            modulations.push_back ({ value.id, parameter, entry->amount });

    if (modulations.size() < 2)
        return modulations;

    std::map<ModulatorID, int> order;
    forEachOwner (song,
                  [&modulation, &order] (const ModulatorPlace& owner)
                  {
                      if (const auto* list = modulation.findList (owner))
                          for (const auto id : *list)
                              order.emplace (id, (int) order.size());
                  });

    std::sort (modulations.begin(), modulations.end(),
               [&order] (const Modulation& a, const Modulation& b)
               {
                   const auto first = order.find (a.modulator), second = order.find (b.modulator);
                   return (first != order.end() ? first->second : (int) order.size())
                          < (second != order.end() ? second->second : (int) order.size());
               });
    return modulations;
}

bool isMacro (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() && value->isMacro();
}

std::vector<ModulatorID> getContainerMacros (const Song& song, DeviceID container)
{
    std::vector<ModulatorID> ids;

    if (container.isValid())
        for (const auto& macro : getOwnedModulators (song, { .container = container }))
            if (macro.isMacro())
                ids.push_back (macro.id);

    return ids;
}

bool hasModulations (const Song& song, TrackID track)
{
    if (! track.isValid())
        return false;

    // Its own and its containers': each on its channel.
    const auto channel = getChannelPlace (song, track);
    const auto modulation = getArea (song);

    return std::any_of (modulation.modulators.begin(), modulation.modulators.end(), [&channel] (const auto& each)
                        { return isOnChannel (each.second.place, channel) && ! each.second.modulations.empty(); });
}

std::optional<ModulatorType> findModulatorType (const juce::String& name)
{
    for (size_t i = 0; i < std::size (schema::preset::modulatorTypeNames); ++i)
        if (name == schema::preset::modulatorTypeNames[i])
            return (ModulatorType) i;

    return {};
}

ModulatorType getModulatorType (const ModulatorValue& value)
{
    if (! value.kind.has_value())
        return ModulatorType::macro;

    switch (*value.kind)
    {
        case devices::ModulatorKind::envelopeFollower:
            return ModulatorType::envelopeFollower;
        case devices::ModulatorKind::steps:
            return ModulatorType::steps;
        case devices::ModulatorKind::random:
            return ModulatorType::random;
        case devices::ModulatorKind::breakpoints:
            return ModulatorType::breakpoints;
        case devices::ModulatorKind::noteFollower:
            return ModulatorType::noteFollower;
        case devices::ModulatorKind::lfo:
            break;
    }

    return ModulatorType::lfo;
}

bool isModulatorOn (const ModulatorValue& value)
{
    return value.isMacro() || ! value.off;
}

bool isBipolar (const ModulatorValue& value)
{
    if (! value.kind.has_value())
        return false;

    switch (*value.kind)
    {
        case devices::ModulatorKind::lfo:
        case devices::ModulatorKind::random:
        case devices::ModulatorKind::breakpoints:
            return value.getParameterValue (devices::findModulatorParameter (*value.kind, "bipolar")) >= 0.5f;
        case devices::ModulatorKind::steps:
            return true; // steps go from -1 to 1
        case devices::ModulatorKind::envelopeFollower:
        case devices::ModulatorKind::noteFollower:
            break;
    }

    return false;
}

int getNumSteps (const ModulatorValue& value)
{
    if (value.kind != devices::ModulatorKind::steps)
        return 1;

    const auto numSteps = value.getParameterValue (devices::findModulatorParameter (devices::ModulatorKind::steps, "numSteps"));
    return juce::jlimit (1, devices::Modulator::maxSteps, roundToNearestInt (numSteps, 1));
}

//==============================================================================
std::vector<ModulatorID> getModulators (const Song& song, TrackID track)
{
    std::vector<ModulatorID> modulators, macros;

    for (const auto& each : getOwnedModulators (song, getChannelPlace (song, track)))
        (each.isMacro() ? macros : modulators).push_back (each.id);

    modulators.insert (modulators.end(), macros.begin(), macros.end());
    return modulators;
}

std::optional<ModulatorType> getModulatorType (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() ? std::optional (getModulatorType (*value)) : std::nullopt;
}

TrackID getModulatorTrack (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);

    if (! value.has_value())
        return {};

    // The master's by its own ID, as a track's (Schema.h: MASTER).
    return value->place.onMaster ? getTrackID (findChainState (song, {})) : value->place.track;
}

DeviceID getModulatorContainer (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() ? value->place.container : DeviceID();
}

juce::String getModulatorName (const Song& song, ModulatorID id)
{
    // Its value looked up once; its owner's others before it only by their kinds: a channel's slots ask each of its
    // modulators' names at each refresh.
    const auto modulation = getArea (song);
    const auto* value = modulation.find (id);

    if (value == nullptr)
        return {};

    // A macro has a name of its own, "Macro 1" at first.
    if (value->isMacro() && value->name.isNotEmpty())
        return value->name;

    // Its place among its owner's of its kind.
    int number = 0;

    if (const auto* list = modulation.findList (value->place))
    {
        for (const auto each : *list)
        {
            if (const auto* other = modulation.find (each); other != nullptr && other->kind == value->kind)
                ++number;

            if (each == id)
                break;
        }
    }

    return getModulatorTypeName (getModulatorType (*value)) + " " + juce::String (number);
}

//==============================================================================
bool setModulator (Song& song, const ModulatorValue& value, int index)
{
    // Where its place says, whatever it is (what a file holds and Phazite never makes, a macro on a folder, stays as it is: a
    // version of it, a merge's choice), a container's macro on the channel its container is on; nothing for a place the song
    // hasn't.
    auto* values = song.getValues();
    const auto place = findOwnerPlace (song, value.place);

    if (values == nullptr || ! value.id.isValid() || ! place.has_value())
        return false;

    auto next = value;
    next.place = *place;
    return values->setModulator (song, std::move (next), index);
}

bool eraseModulator (Song& song, ModulatorID id)
{
    auto* values = song.getValues();
    return values != nullptr && values->eraseModulator (song, id);
}

bool setModulatorOrder (Song& song, const ModulatorPlace& owner, const immer::flex_vector<ModulatorID>& order)
{
    auto* values = song.getValues();
    const auto* list = values != nullptr ? values->modulation.get().findList (owner) : nullptr;

    if (list == nullptr)
        return false;

    // Those named, in their order (each once), then the others in theirs.
    std::vector<ModulatorID> held (list->begin(), list->end()), wanted;
    const auto isWanted = [&wanted] (ModulatorID id) { return std::find (wanted.begin(), wanted.end(), id) != wanted.end(); };

    for (const auto id : order)
        if (std::find (held.begin(), held.end(), id) != held.end() && ! isWanted (id))
            wanted.push_back (id);

    for (const auto id : held)
        if (! isWanted (id))
            wanted.push_back (id);

    return values->setModulatorOrder (song, owner, immer::flex_vector<ModulatorID> (wanted.begin(), wanted.end()));
}

bool setAllModulation (Song& song, SongModulation next)
{
    auto* values = song.getValues();
    return values != nullptr && values->setAllModulation (song, std::move (next));
}

bool eraseModulatorsOf (Song& song, const std::vector<ModulatorPlace>& owners)
{
    auto* values = song.getValues();

    if (values == nullptr)
        return false;

    std::vector<ModulatorID> gone;

    for (const auto& owner : owners)
        if (const auto* list = values->modulation.get().findList (owner))
            gone.insert (gone.end(), list->begin(), list->end());

    return ! gone.empty() && values->eraseModulators (song, gone);
}

void takeModulatorsOf (Song& song, const juce::ValueTree& owner)
{
    if (song.getValues() == nullptr)
        return;

    for (const auto& element : owner)
    {
        if (! element.hasType (schema::MODULATOR) && ! element.hasType (schema::MACRO))
            continue;

        if (auto value = readHeldModulator (element, readModulatorPlace (element));
            value.has_value() && song.getValues()->modulation.get().find (value->id) == nullptr)
            setModulator (song, *value);
    }
}

std::optional<ModulatorPlace> findOwnerPlace (const Song& song, const ModulatorPlace& owner)
{
    // A container's: on the channel the devices' place of it names (Mixer.h: findContainerPlace(), read without the
    // container's value), or the place as given for one whose channel its place doesn't name (a container in another's
    // chain); a channel's own as given, while the song has its element.
    if (owner.container.isValid())
    {
        const auto container = findContainerPlace (song, owner.container);

        if (! container.has_value())
            return std::nullopt;

        if (! container->onMaster && ! container->track.isValid())
            return owner;

        return ModulatorPlace { .track = container->track, .onMaster = container->onMaster, .container = owner.container };
    }

    if (! (owner.onMaster ? findChainState (song, {}) : findTrackState (song, owner.track)).isValid())
        return std::nullopt;

    return owner;
}

//==============================================================================
ModulatorID addModulator (Song& song, TrackID track, ModulatorType type)
{
    const auto channel = track.isValid() ? findChainState (song, track) : juce::ValueTree();

    if (type == ModulatorType::macro)
    {
        if (! channel.hasType (schema::TRACK) || ! canHoldMacros (channel))
            return {};

        const auto owned = getOwnedModulators (song, getChannelPlace (song, track));
        const auto count = std::count_if (owned.begin(), owned.end(), [] (const ModulatorValue& each) { return each.isMacro(); });
        return addMacro (song, track, "Macro " + juce::String ((int) count + 1));
    }

    if (! canHoldModulators (channel))
        return {};

    ModulatorValue value;
    value.id = ModulatorID::fromRaw (song.makeID());
    value.place = getChannelPlace (song, track);
    value.kind = (devices::ModulatorKind) type; // ModulatorType's order is the kinds' (ModulationValue.cpp)

    // LFOs and random sources start going both ways, where their polarity's default has them going one way only. Step
    // sequencers and envelope followers start at full depth, their default, which the song keeps no value for.
    if (type == ModulatorType::lfo || type == ModulatorType::random)
    {
        const auto index = devices::findModulatorParameter (*value.kind, "bipolar");

        if (! isDefaultValue (devices::getModulatorParameters (*value.kind)[(size_t) index], 1.0f))
            value.values = value.values.set ((std::uint32_t) index, 1.0f);
    }

    // A step sequencer's steps rise across as many steps as it starts with.
    if (type == ModulatorType::steps)
    {
        const auto numSteps = getNumSteps (value);
        auto steps = value.steps.transient();

        for (int i = 0; i < numSteps; ++i)
            steps.push_back (numSteps > 1 ? -1.0f + 2.0f * (float) i / (float) (numSteps - 1) : 0.0f);

        value.steps = steps.persistent();
    }

    setModulator (song, value);
    return value.id;
}

ModulatorID addMacro (Song& song, TrackID track, const juce::String& name)
{
    if (const auto channel = track.isValid() ? findChainState (song, track) : juce::ValueTree();
        ! channel.hasType (schema::TRACK) || ! canHoldMacros (channel))
        return {};

    // At 0, so that what it modulates doesn't move until it is turned.
    ModulatorValue value;
    value.id = ModulatorID::fromRaw (song.makeID());
    value.place = getChannelPlace (song, track);
    value.name = name;
    setModulator (song, value);
    return value.id;
}

void renameMacro (Song& song, ModulatorID id, const juce::String& name)
{
    const auto trimmed = name.trim();

    if (auto value = getModulatorValue (song, id); value.has_value() && value->isMacro() && trimmed.isNotEmpty())
    {
        value->name = trimmed;
        setModulator (song, *value);
    }
}

void removeModulator (Song& song, ModulatorID id)
{
    // With what it modulates; and what names it goes with it (a MIDI link of a macro's value or of a modulator's parameter),
    // as with every other removal (known bug 241).
    if (eraseModulator (song, id))
        forgetWhatNamesTheGone (song);
}

bool isBipolar (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() && isBipolar (*value);
}

bool isModulatorOn (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() && isModulatorOn (*value);
}

void setModulatorOn (Song& song, ModulatorID id, bool on)
{
    if (auto value = getModulatorValue (song, id); value.has_value() && ! value->isMacro() && isModulatorOn (*value) != on)
    {
        value->off = ! on;
        setModulator (song, *value);
    }
}

void setModulatorStep (Song& song, ModulatorID id, int index, float step)
{
    auto value = getModulatorValue (song, id);

    if (! value.has_value() || value->isMacro() || ! juce::isPositiveAndBelow (index, devices::Modulator::maxSteps))
        return;

    std::vector<float> steps (value->steps.begin(), value->steps.end());
    steps.resize (std::max (steps.size(), (size_t) index + 1), 0.0f);
    steps[(size_t) index] = juce::jlimit (-1.0f, 1.0f, step);

    // Without the zeros at the end, as the song holds them (ModulationValue.h: ModulatorValue::steps).
    while (! steps.empty() && std::abs (steps.back()) < 1.0e-4f)
        steps.pop_back();

    value->steps = immer::flex_vector<float> (steps.begin(), steps.end());
    setModulator (song, *value);
}

// A control's drag sets these at the mouse's rate: the value looked up once, its one value changed, and set (nothing for the
// value it holds, which makes no undo step).
void setModulatorValueSetByHand (Song& song, ModulatorID id, const devices::ParameterInfo& info, float set)
{
    auto value = getModulatorValue (song, id);
    const auto index = value.has_value() && value->kind.has_value() ? devices::findModulatorParameter (*value->kind, info.id) : -1;

    if (index < 0)
        return;

    // None at its default, as the song holds a modulator's values (ModulationValue.h).
    const auto* held = value->values.find ((std::uint32_t) index);
    const auto atDefault = isDefaultValue (info, set);

    if (atDefault ? held == nullptr : (held != nullptr && juce::exactlyEqual (*held, set)))
        return;

    value->values = atDefault ? value->values.erase ((std::uint32_t) index) : value->values.set ((std::uint32_t) index, set);
    setModulator (song, *value);
}

void setMacroValue (Song& song, ModulatorID id, float set)
{
    if (auto value = getModulatorValue (song, id); value.has_value() && value->isMacro() && ! juce::exactlyEqual (value->value, set))
    {
        value->value = set;
        setModulator (song, *value);
    }
}

//==============================================================================
std::vector<Modulation> getModulations (const Song& song, const ParameterRef& parameter)
{
    if (! locateParameter (song, parameter).has_value())
        return {};

    return findModulationsOf (song, parameter);
}

std::vector<Modulation> getModulations (const Song& song, ModulatorID id)
{
    std::vector<Modulation> modulations;

    if (const auto value = getModulatorValue (song, id))
        for (const auto& modulation : value->modulations)
            if (locateParameter (song, modulation.parameter).has_value())
                modulations.push_back ({ id, modulation.parameter, modulation.amount });

    return modulations;
}

bool canModulate (const Song& song, ModulatorID id, const ParameterRef& parameter)
{
    const auto value = getModulatorValue (song, id);
    const auto found = value.has_value() ? locateParameter (song, parameter) : std::nullopt;

    // A modulator's own parameters and a macro's value are set by hand.
    if (! found.has_value() || found->source == ParameterSource::modulator || found->source == ParameterSource::macro)
        return false;

    // A container's macros modulate the devices in it, and its chains' volumes, pans and crossovers.
    if (const auto container = value->place.container; container.isValid())
    {
        if (found->source == ParameterSource::device || found->source == ParameterSource::plugin)
            return findContainerOf (song, parameter.device) == container;

        return (found->source == ParameterSource::chainVolume || found->source == ParameterSource::chainPan
                || found->source == ParameterSource::crossover)
               && getChainContainer (song, parameter.device) == container;
    }

    const auto track = getModulatorTrack (song, id);
    return track.isValid() && getParameterTrack (song, parameter) == track;
}

void setModulation (Song& song, ModulatorID id, const ParameterRef& parameter, float amount)
{
    auto value = getModulatorValue (song, id);

    if (! value.has_value() || ! parameter.isValid())
        return;

    // A modulation it has keeps its place; one it hasn't goes after the others.
    const ModulationEntry entry { .parameter = parameter, .amount = (float) writeAmount (amount) };
    auto modulations = value->modulations.push_back (entry);

    for (size_t i = 0; i < value->modulations.size(); ++i)
    {
        if (value->modulations[i].parameter == parameter)
        {
            modulations = value->modulations.set (i, entry);
            break;
        }
    }

    value->modulations = modulations;
    setModulator (song, *value);
}

void removeModulation (Song& song, ModulatorID id, const ParameterRef& parameter)
{
    auto value = getModulatorValue (song, id);

    if (! value.has_value() || value->findModulation (parameter) == nullptr)
        return;

    auto kept = immer::flex_vector<ModulationEntry>().transient();

    for (const auto& modulation : value->modulations)
        if (! (modulation.parameter == parameter))
            kept.push_back (modulation);

    value->modulations = kept.persistent();
    setModulator (song, *value);
}

int forgetModulationsOf (Song& song, const std::function<bool (DeviceID)>& gone)
{
    // Each modulator's modulations looked at, and those of what is gone taken out, the song's modulation set once.
    const auto held = getArea (song);
    auto next = held;
    auto forgotten = 0;

    for (const auto& [key, value] : held.modulators)
    {
        if (std::none_of (value.modulations.begin(), value.modulations.end(),
                          [&gone] (const ModulationEntry& entry) { return gone (entry.parameter.device); }))
            continue;

        auto changed = value;
        auto kept = immer::flex_vector<ModulationEntry>().transient();

        for (const auto& modulation : value.modulations)
            if (! gone (modulation.parameter.device))
                kept.push_back (modulation);

        forgotten += (int) (value.modulations.size() - kept.size());
        changed.modulations = kept.persistent();
        next = withHeldModulator (next, std::move (changed));
    }

    if (forgotten > 0)
        setAllModulation (song, std::move (next));

    return forgotten;
}

void renameModulated (Song& song, const ParameterRef& from, const ParameterRef& to)
{
    // In place: the entry's parameter changed where it is among its modulator's (a modulation set anew would go after the
    // others), the song's modulation set once.
    const auto held = getArea (song);
    auto next = held;

    for (const auto& [key, value] : held.modulators)
    {
        for (size_t i = 0; i < value.modulations.size(); ++i)
        {
            if (value.modulations[i].parameter == from)
            {
                auto changed = value;
                changed.modulations = value.modulations.set (i, { .parameter = to, .amount = value.modulations[i].amount });
                next = withHeldModulator (next, std::move (changed));
                break;
            }
        }
    }

    setAllModulation (song, std::move (next));
}

void copyModulations (Song& song, const std::map<std::uint64_t, std::uint64_t>& copied)
{
    // Each modulator's modulations of the originals, each of its copy too where it modulates none of that copy's parameter
    // yet, after the others, in their order: the song's modulation worked out whole and set once.
    auto next = getArea (song);
    auto changed = false;

    for (auto value : getEveryModulator (song))
    {
        auto modulations = value.modulations;

        for (const auto& modulation : value.modulations)
        {
            const auto found = copied.find (modulation.parameter.device.getRaw());

            if (found == copied.end())
                continue;

            if (const ParameterRef copy { DeviceID::fromRaw (found->second), modulation.parameter.parameter };
                value.findModulation (copy) == nullptr)
                modulations = modulations.push_back ({ .parameter = copy, .amount = modulation.amount });
        }

        if (modulations.size() != value.modulations.size())
        {
            value.modulations = modulations;
            next = withHeldModulator (next, std::move (value));
            changed = true;
        }
    }

    if (changed)
        setAllModulation (song, std::move (next));
}

void copyModulators (Song& song, TrackID from, TrackID to, CopiedIDs& copied)
{
    const auto target = to.isValid() ? findChainState (song, to) : juce::ValueTree();

    if (! from.isValid() || ! target.isValid())
        return;

    const auto place = getChannelPlace (song, to);
    std::vector<ModulatorValue> copies;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> copiedModulators;

    for (const auto& modulator : getOwnedModulators (song, getChannelPlace (song, from)))
    {
        if (! (modulator.isMacro() ? canHoldMacros (target) : canHoldModulators (target)))
            continue;

        // As it is set, with what it modulates of the originals alone, which its copy modulates of their copies.
        auto modulations = immer::flex_vector<ModulationEntry>().transient();

        for (const auto& modulation : modulator.modulations)
            if (const auto found = copied.find (modulation.parameter.device.getRaw()); found != copied.end())
                modulations.push_back (
                    { .parameter = { DeviceID::fromRaw (found->second), modulation.parameter.parameter }, .amount = modulation.amount });

        if (modulations.size() == 0)
            continue;

        auto copy = modulator;
        copy.id = ModulatorID::fromRaw (song.makeID());
        copy.place = place;
        copy.modulations = modulations.persistent();
        copiedModulators.emplace_back (modulator.id.getRaw(), copy.id.getRaw());
        copies.push_back (std::move (copy));
    }

    if (copies.empty())
        return;

    // Each copy's ID among the copied, once all of them are worked out, for what names a modulator's or a macro's parameter
    // (a macro's value's automation: Automation.h, copyCurves(), after this).
    for (const auto& [original, copy] : copiedModulators)
        copied[original] = copy;

    auto next = getArea (song);

    for (auto& copy : copies)
        next = withHeldModulator (next, std::move (copy));

    setAllModulation (song, std::move (next));
}

void copyModulation (Song& song, CopiedIDs& copied)
{
    // The owners copied whose copies hold none, by the song's modulation's lists alone, none read: each original's own (a
    // track's, a container's), its copy's place (a container's copy on its channel's copy, or on the channel itself).
    auto next = getArea (song);
    std::vector<std::pair<ModulatorPlace, std::vector<ModulatorValue>>> owners;

    const auto ownedBy = [&next] (const immer::flex_vector<ModulatorID>& list)
    {
        std::vector<ModulatorValue> owned;

        for (const auto id : list)
            if (const auto* value = next.find (id))
                owned.push_back (*value);

        return owned;
    };

    for (const auto& [original, copy] : copied)
    {
        if (const auto* list = next.channels.find (original); list != nullptr && next.channels.find (copy) == nullptr)
            owners.emplace_back (ModulatorPlace { .track = TrackID::fromRaw (copy) }, ownedBy (*list));

        if (const auto* list = next.containers.find (original); list != nullptr && next.containers.find (copy) == nullptr)
        {
            auto owned = ownedBy (*list);

            if (owned.empty())
                continue;

            auto place = owned.front().place;
            place.container = DeviceID::fromRaw (copy);

            if (const auto channel = copied.find (place.track.getRaw()); ! place.onMaster && channel != copied.end())
                place.track = TrackID::fromRaw (channel->second);

            owners.emplace_back (place, std::move (owned));
        }
    }

    if (owners.empty())
        return;

    // Each a new ID, in the copied IDs, before any modulation of one is named by them.
    std::vector<ModulatorValue> copies;

    for (const auto& [place, list] : owners)
    {
        for (auto copy : list)
        {
            const auto id = copy.id;
            copy.id = ModulatorID::fromRaw (song.makeID());
            copy.place = place;
            copied[id.getRaw()] = copy.id.getRaw();
            copies.push_back (std::move (copy));
        }
    }

    for (auto& copy : copies)
    {
        auto modulations = immer::flex_vector<ModulationEntry>().transient();

        for (auto modulation : copy.modulations)
        {
            if (const auto found = copied.find (modulation.parameter.device.getRaw()); found != copied.end())
                modulation.parameter.device = DeviceID::fromRaw (found->second);

            modulations.push_back (modulation);
        }

        copy.modulations = modulations.persistent();
        next = withHeldModulator (next, std::move (copy));
    }

    setAllModulation (song, std::move (next));
}

//==============================================================================
AreaChoice chooseModulationClash (Song& song, const MergeClash& clash, int value, const MergeSides& sides)
{
    // A path through a modulator or a macro ("TRACKS:", "TRACK:12", "MODULATOR:93", "MODULATES:1042/cutoff"): modulators and
    // macros merge as items by their IDs, each property of theirs at a time, what they modulate by its parameter and their
    // values set by hand by their names (SongMerge.cpp); so a clash is a property of one, or one whole (added or taken out on
    // a side), or a property of what is in one (a modulation's amount, a value set by hand), or what is in one whole.
    int step = -1;

    for (int i = 0; i < clash.path.size() && step < 0; ++i)
        if (clash.path[i].startsWith (schema::MODULATOR.toString() + ":") || clash.path[i].startsWith (schema::MACRO.toString() + ":"))
            step = i;

    if (step < 0)
        return AreaChoice::notTheArea;

    // Refused (nothing written) for what a merge of two songs Phazite holds never makes, but a commit's song may come from
    // anyone (tests/FuzzTests.cpp chooses every clash of a damaged merge): a path deeper than what is in one, one whose owner
    // the song hasn't (as the tree's choice found no place for it), one the song holds as the other kind or on another owner,
    // its ID, a property of what is in one that is its key (a modulation's parameter) or that it can't hold (a value of a
    // parameter its kind hasn't), an element of another type or key, one that stands in for what its version couldn't read
    // (SongPieces.h: holdsStandIn()), and one the song would read otherwise than it was chosen (ModulationValue.h:
    // readHeldModulator()).
    if (step == 0 || clash.path.size() > step + 2 || ! juce::isPositiveAndBelow (value, 3))
        return AreaChoice::refused;

    auto ownerPath = clash.path;
    ownerPath.removeRange (step, clash.path.size() - step);
    const auto place = readOwnerPlace (findByPath (song.getState(), ownerPath));
    const auto isMacro = clash.path[step].startsWith (schema::MACRO.toString() + ":");
    const auto id = ModulatorID::fromRaw (readID (clash.path[step].fromFirstOccurrenceOf (":", false, false)));
    const auto held = getModulatorValue (song, id);

    if (! place.has_value() || ! id.isValid() || (held.has_value() && (! isSameOwner (held->place, *place) || held->isMacro() != isMacro)))
        return AreaChoice::refused;

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };

    // One's own property: the value chosen given to it (ModulationValue.h: setModulatorProperty(), a void value taking it
    // away as the tree did).
    if (clash.path.size() == step + 1 && clash.property.isValid())
    {
        if (! held.has_value() || clash.property == schema::modulatorID)
            return AreaChoice::refused;

        auto next = *held;

        if (const auto read = setModulatorProperty (next, clash.property, *values[value])
                                  ? readHeldModulator (writeModulatorElement (next), next.place)
                                  : std::nullopt;
            ! read.has_value() || ! (*read == next))
            return AreaChoice::refused;

        setModulator (song, next);
        return AreaChoice::chosen;
    }

    // One whole: the side chosen's (read as the song holds one) made the song's, in its place or after its owner's others,
    // or the song's taken out where that side had none.
    if (clash.path.size() == step + 1)
    {
        const auto& chosen = *elements[value];

        if (! chosen.isValid())
        {
            eraseModulator (song, id);
            return AreaChoice::chosen;
        }

        const auto read =
            getPathStep (chosen) == clash.path[step] && ! holdsStandIn (chosen) ? readHeldModulator (chosen, *place) : std::nullopt;

        if (! read.has_value())
            return AreaChoice::refused;

        setModulator (song, *read);
        return AreaChoice::chosen;
    }

    // What is in one (a MODULATES, a modulator's VALUES): chosen in an element of the one the song holds, as the tree's choice
    // chose it in the song's, and the element read as the song holds one.
    const auto& inPath = clash.path[step + 1];
    const auto isModulates = inPath.startsWith (schema::MODULATES.toString() + ":");

    if (! held.has_value() || ! (isModulates || (inPath.startsWith (schema::VALUES.toString() + ":") && ! held->isMacro())))
        return AreaChoice::refused;

    auto element = writeModulatorElement (*held);
    juce::ValueTree inside;

    for (const auto& child : element)
        if (getPathStep (child) == inPath)
            inside = child;

    if (clash.property.isValid())
    {
        if (isModulates ? clash.property != schema::modulatesAmount
                        : devices::findModulatorParameter (*held->kind, clash.property.toString().toStdString()) < 0)
            return AreaChoice::refused;

        // A modulation the side chosen holds none of, which both lines made (the property's clash has no value before), taken
        // away rather than left without its amount (known bug 239's shape), where that side's song can be read; and one the
        // song holds none of (an earlier choice took it away) put back first as the side chosen holds it.
        const juce::ValueTree* songs[] = { &sides.base, &sides.before, &sides.theirs };
        const auto& side = *songs[value];
        const auto canRead = side.hasType (schema::PHAZITE);
        const auto there = canRead ? findByPath (side, clash.path) : juce::ValueTree();

        if (isModulates && canRead && ! there.isValid())
        {
            if (inside.isValid())
                element.removeChild (inside, nullptr);
        }
        else
        {
            if (! inside.isValid())
            {
                if (isModulates && (! there.isValid() || holdsStandIn (there)))
                    return AreaChoice::refused;

                inside = isModulates ? there.createCopy() : juce::ValueTree (schema::VALUES);
                element.appendChild (inside, nullptr);
            }

            if (values[value]->isVoid())
                inside.removeProperty (clash.property, nullptr);
            else
                inside.setProperty (clash.property, *values[value], nullptr);
        }
    }
    else
    {
        const auto& chosen = *elements[value];

        if (chosen.isValid() && (getPathStep (chosen) != inPath || holdsStandIn (chosen)))
            return AreaChoice::refused;

        const auto at = element.indexOf (inside);

        if (inside.isValid())
            element.removeChild (inside, nullptr);

        if (chosen.isValid())
            element.addChild (chosen.createCopy(), at, nullptr);
    }

    const auto next = readHeldModulator (element, held->place);

    if (! next.has_value())
        return AreaChoice::refused;

    setModulator (song, *next);
    return AreaChoice::chosen;
}
} // namespace phazite
