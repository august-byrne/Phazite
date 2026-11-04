// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginInstances.h"
#include "internal/PluginHost.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"

#include <set>

namespace phazite
{
namespace
{
    constexpr double defaultSampleRate = 44100.0;
    constexpr int openBlockSize = 512; // what a new instance is opened for, before the engine prepares it

    // Sets a property as ValueTree::setProperty() does with an undo manager, which leaves a value that is the same as JUCE
    // compares values alone (a number read from a file is text), but without one.
    void storeProperty (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = state.getPropertyPointer (property); stored == nullptr || *stored != value)
            state.setProperty (property, value, nullptr);
    }

    juce::MemoryBlock readPluginState (const juce::ValueTree& device)
    {
        juce::MemoryBlock block;

        if (const auto text = device[schema::pluginState].toString(); text.isNotEmpty())
            block.fromBase64Encoding (text);

        return block;
    }

    // The plug-ins' elements in a part of the song (a track's element, with its containers'), in their order.
    void findPlugins (const juce::ValueTree& tree, std::vector<juce::ValueTree>& found)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::DEVICE))
            {
                if (isPluginState (child))
                    found.push_back (child);
            }
            else if (child.hasType (schema::CONTAINER))
            {
                findPlugins (child, found);
            }
        }
    }

    std::vector<juce::ValueTree> findPlugins (const Song& song)
    {
        std::vector<juce::ValueTree> found;

        for (const auto& track : getTrackStates (song))
            findPlugins (track, found);

        return found;
    }

    // The device a VALUE, a plug-in's DEVICE or a property of one is of, if it is a plug-in's.
    juce::ValueTree findPluginOf (juce::ValueTree tree)
    {
        if (tree.hasType (schema::VALUE))
            tree = tree.getParent();

        return tree.hasType (schema::DEVICE) && isPluginState (tree) ? tree : juce::ValueTree();
    }
} // namespace

PluginInstances::PluginInstances (Song& s, PluginHost& h, double rate)
    : song (s),
      host (h),
      sampleRate (rate > 0.0 ? rate : defaultSampleRate),
      state (s.getState())
{
    song.setDeviceStateStore (
        [this] (std::uint64_t track)
        {
            if (track == 0)
                storeAll();
            else
                storeTrack (TrackID::fromRaw (track));
        });

    state.addListener (this);
    host.addChangeListener (this);
    sync();
}

PluginInstances::~PluginInstances()
{
    host.removeChangeListener (this);
    state.removeListener (this);
    song.setDeviceStateStore ({});

    // Told of nothing more: what plays them may hold them a little longer.
    for (auto& [id, entry] : entries)
    {
        if (entry.instance != nullptr)
        {
            entry.instance->onParameterChanged = nullptr;
            entry.instance->onGesture = nullptr;
            entry.instance->onStateChanged = nullptr;
            entry.instance->onLatencyChanged = nullptr;
        }
    }
}

void PluginInstances::setSampleRate (double rate)
{
    if (rate > 0.0)
        sampleRate = rate;
}

std::shared_ptr<SandboxedInstance> PluginInstances::find (DeviceID id)
{
    if (const auto found = entries.find (id); found != entries.end())
        return found->second.instance;

    const auto device = findDeviceState (song, id);

    if (! isPluginState (device))
        return nullptr;

    auto* entry = open (id, device);
    return entry != nullptr ? entry->instance : nullptr;
}

PluginInstances::Entry* PluginInstances::open (DeviceID id, const juce::ValueTree& device)
{
    auto& entry = entries[id];
    entry.instance = nullptr;
    entry.error = {};
    const auto known = findKnownPlugin (host, getPluginIdentity (device));
    entry.wasKnown = known.has_value();

    if (! host.loadsPlugins())
    {
        entry.unloaded = Status::off;
        entry.error = "Plug-ins are off after Phazite closed unexpectedly.";
        return &entry;
    }

    if (! known.has_value())
    {
        entry.unloaded = Status::missing;
        entry.error = getPluginIdentity (device).name + " isn't installed on this computer.";
        return &entry;
    }

    // From the state the song keeps of it, then its program, if the state left another; then the values the song sets by
    // hand under its automation.
    std::unique_ptr<SandboxedInstance> made =
        host.createInstance (*known, readPluginState (device), sampleRate, openBlockSize, entry.error);

    if (made == nullptr)
    {
        entry.unloaded = host.hasCrashedWhileLoading (*known) ? Status::crashedWhileLoading : Status::missing;
        return &entry;
    }

    if (const auto* program = device.getPropertyPointer (schema::pluginProgram); program != nullptr && made->getNumPrograms() > 1)
        if (const auto wanted = (int) *program; wanted != made->getCurrentProgram())
            made->setCurrentProgram (wanted);

    for (const auto& child : device)
        if (child.hasType (schema::VALUE))
            if (const auto index = made->findParameter (child[schema::valueParameter].toString()); index >= 0)
                made->setParameterValue (index, (float) child[schema::valueSetByHand]);

    entry.instance = std::move (made);
    listen (id, *entry.instance);
    return &entry;
}

void PluginInstances::listen (DeviceID id, SandboxedInstance& instance)
{
    const auto refOf = [&instance, id] (int parameter)
    {
        const auto& infos = instance.getParameterInfos();
        return juce::isPositiveAndBelow (parameter, (int) infos.size()) ? ParameterRef { id, infos[(size_t) parameter].id }
                                                                        : ParameterRef {};
    };

    // A value it set itself: a VALUE of the song's takes it in, without an undo step, and the song tells what listens to it.
    instance.onParameterChanged = [this, id, refOf] (int parameter, float value)
    {
        const auto ref = refOf (parameter);

        if (! ref.isValid())
            return;

        if (auto device = findDeviceState (song, id); device.isValid())
        {
            for (auto child : device)
            {
                if (child.hasType (schema::VALUE) && child[schema::valueParameter].toString() == ref.parameter)
                {
                    const juce::ScopedValueSetter<bool> writing (storing, true);
                    storeProperty (child, schema::valueSetByHand, value);
                }
            }
        }

        // A change of its own (the helper tells none it was given), which the next save stores: an unsaved change.
        song.markChangedOutsideHistory();
        song.tellValueSetByHand (ref, value, true);
    };

    instance.onGesture = [this, refOf] (int parameter, bool starting)
    {
        if (const auto ref = refOf (parameter); ref.isValid() && listener != nullptr)
            listener->pluginGestureChanged (ref, starting);
    };

    // Its own settings changed: an unsaved change, which the next save stores.
    instance.onStateChanged = [this] { song.markChangedOutsideHistory(); };

    instance.onLatencyChanged = [this]
    {
        if (listener != nullptr)
            listener->pluginLatencyChanged();
    };
}

void PluginInstances::sync()
{
    // Those the song no longer holds go (what plays them may hold them until its plan is handed back).
    std::set<DeviceID> held;

    for (const auto& device : findPlugins (song))
        held.insert (getDeviceID (device));

    std::erase_if (entries, [&held] (const auto& entry) { return ! held.contains (entry.first); });

    for (const auto& device : findPlugins (song))
        if (const auto id = getDeviceID (device); ! entries.contains (id))
            open (id, device);
}

std::optional<PluginInstances::Status> PluginInstances::getStatus (DeviceID id)
{
    if (! isPluginState (findDeviceState (song, id)))
        return {};

    const auto instance = find (id);

    if (instance != nullptr)
        return instance->hasCrashed() ? Status::crashed : Status::playing;

    const auto found = entries.find (id);
    return found != entries.end() ? found->second.unloaded : Status::missing;
}

bool PluginInstances::reload (DeviceID id, juce::String& error)
{
    const auto device = findDeviceState (song, id);
    const auto status = getStatus (id);

    if (! status.has_value() || (*status != Status::crashed && *status != Status::crashedWhileLoading))
        return false;

    // One whose helper went opens in a new one, the same instance; one that crashed as it loaded loads again.
    if (*status == Status::crashed)
    {
        if (! host.reload (*entries[id].instance, error))
            return false;
    }
    else
    {
        if (const auto known = findKnownPlugin (host, getPluginIdentity (device)))
            host.forgetCrashWhileLoading (*known);

        const auto generation = entries[id].generation;
        auto* entry = open (id, device);
        entries[id].generation = generation;

        if (entry == nullptr || entry->instance == nullptr)
        {
            error = entry != nullptr ? entry->error : juce::String();
            return false;
        }
    }

    ++entries[id].generation;
    return true;
}

int PluginInstances::getGeneration (DeviceID id) const
{
    const auto found = entries.find (id);
    return found != entries.end() ? found->second.generation : 0;
}

void PluginInstances::store (DeviceID id)
{
    const auto found = entries.find (id);
    auto device = findDeviceState (song, id);

    if (found == entries.end() || found->second.instance == nullptr || ! device.isValid())
        return;

    const juce::ScopedValueSetter<bool> writing (storing, true);
    auto& instance = *found->second.instance;

    // Its settings, as it gives them now (or as it last had them, if its helper went); and its program.
    juce::MemoryBlock block;
    instance.getStateInformation (block);

    if (block.getSize() > 0)
        storeProperty (device, schema::pluginState, block.toBase64Encoding());
    else
        device.removeProperty (schema::pluginState, nullptr);

    if (instance.getNumPrograms() > 1)
        storeProperty (device, schema::pluginProgram, instance.getCurrentProgram());
    else
        device.removeProperty (schema::pluginProgram, nullptr);

    // Its values set by hand that automation or modulation moves, which its settings don't hold as set by hand (the engine
    // plays the moved ones): each a VALUE, as the song keeps them. Those set by Phazite's controls are there already.
    auto* session = findSession (song);
    const auto* node = session != nullptr ? session->getPlayer().findDevice ((std::int64_t) id.getRaw()) : nullptr;

    if (node == nullptr)
        return;

    const auto& infos = instance.getParameterInfos();

    for (size_t i = 0; i < infos.size(); ++i)
    {
        const ParameterRef ref { id, infos[i].id };

        if (! session->getPlayer().isMoved (ref))
            continue;

        auto value = device.getChildWithProperty (schema::valueParameter, infos[i].id);

        if (! value.isValid())
        {
            value = juce::ValueTree (schema::VALUE, { { schema::valueParameter, infos[i].id } });
            device.appendChild (value, nullptr);
        }

        storeProperty (value, schema::valueSetByHand, node->getValue ((int) i));
    }
}

void PluginInstances::storeTrack (TrackID track)
{
    auto tracks = getTracksInFolder (song, track);
    tracks.insert (tracks.begin(), track);

    for (const auto id : tracks)
    {
        std::vector<juce::ValueTree> found;
        findPlugins (findTrackState (song, id), found);

        for (const auto& device : found)
            store (getDeviceID (device));
    }
}

void PluginInstances::storeAll()
{
    for (const auto& device : findPlugins (song))
        store (getDeviceID (device));
}

//==============================================================================
void PluginInstances::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The plug-ins scanning found changed, or one crashed or was reloaded: one that wasn't installed may load now (one kept
    // off, or that crashed as it loaded, waits for a reload by hand).
    for (const auto& device : findPlugins (song))
    {
        const auto id = getDeviceID (device);

        if (const auto found = entries.find (id); found != entries.end() && found->second.instance == nullptr)
            if (found->second.unloaded == Status::missing && ! found->second.wasKnown
                && findKnownPlugin (host, getPluginIdentity (device)).has_value())
                open (id, device);
    }
}

void PluginInstances::takeFromSong (const juce::ValueTree& device, const juce::Identifier& property)
{
    // A preset loaded into it, or an undo of one: its program, then its settings, whichever of them changed, so that the
    // settings are never overridden by a program set after them (an undo takes back the settings first).
    const auto found = entries.find (getDeviceID (device));

    if (found == entries.end() || found->second.instance == nullptr
        || (property != schema::pluginProgram && property != schema::pluginState))
        return;

    auto& instance = *found->second.instance;

    if (device.hasProperty (schema::pluginProgram))
        if (const auto wanted = (int) device[schema::pluginProgram]; wanted != instance.getCurrentProgram())
            instance.setCurrentProgram (wanted);

    const auto block = readPluginState (device);
    instance.setStateInformation (block.getData(), (int) block.getSize());
}

void PluginInstances::takeValueFromSong (const juce::ValueTree& value)
{
    const auto device = value.getParent();
    const auto found = entries.find (getDeviceID (device));

    if (found == entries.end() || found->second.instance == nullptr)
        return;

    if (const auto index = found->second.instance->findParameter (value[schema::valueParameter].toString()); index >= 0)
        found->second.instance->setParameterValue (index, (float) value[schema::valueSetByHand]);
}

void PluginInstances::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (storing)
        return;

    if (tree.hasType (schema::VALUE))
    {
        if (findPluginOf (tree).isValid())
            takeValueFromSong (tree);
    }
    else if (property == schema::pluginState || property == schema::pluginProgram)
    {
        if (const auto device = findPluginOf (tree); device.isValid())
            takeFromSong (device, property);
    }
}

void PluginInstances::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (! storing && child.hasType (schema::VALUE) && findPluginOf (child).isValid())
        takeValueFromSong (child);
}

void PluginInstances::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
}

//==============================================================================
SandboxedInstance* findPluginInstance (const Song& song, DeviceID id)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().find (id).get() : nullptr;
}

bool hasCrashed (const Song& song, DeviceID id)
{
    auto* session = findSession (song);
    const auto status = session != nullptr ? session->getPlugins().getStatus (id) : std::nullopt;
    return status == PluginInstances::Status::crashed || status == PluginInstances::Status::crashedWhileLoading;
}

bool reloadPlugin (Song& song, DeviceID id, juce::String& error)
{
    auto* session = findSession (song);

    if (session == nullptr || ! session->getPlugins().reload (id, error))
        return false;

    session->update();
    return true;
}
} // namespace phazite
