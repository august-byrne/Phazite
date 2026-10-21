// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixerModel.h"
#include "internal/Containers.h"
#include "internal/Freeze.h"
#include "internal/LevelSource.h"
#include "internal/MixHandle.h"
#include "internal/Mixer.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

namespace phazite
{
namespace
{
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::Plugin* findDevice (const ProjectContext& context, DeviceID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findPluginForID (*edit, toTracktion (id)).get() : nullptr;
    }

    std::vector<DeviceSlot> makeSlots (const juce::Array<te::Plugin*>& devices)
    {
        std::vector<DeviceSlot> slots;

        for (auto* device : devices)
            slots.push_back (makeDeviceSlot (*device));

        return slots;
    }

    // The tracks that send to a return's bus.
    juce::StringArray getSenderNames (const te::Edit& edit, int bus)
    {
        juce::StringArray names;

        for (auto* track : te::getAudioTracks (edit))
            for (auto* send : phazite::getSends (*track))
                if (send->getBusNumber() == bus)
                    names.addIfNotAlreadyThere (track->getName());

        return names;
    }

    // Whether one channel's sound reaches another's: through a send to a return, or by being in a bus folder.
    bool reaches (const te::Track& from, const te::Track& to)
    {
        if (const auto bus = getReturnBus (to); bus >= 0)
            for (auto* send : phazite::getSends (from))
                if (send->getBusNumber() == bus)
                    return true;

        for (auto* parent = from.getParentFolderTrack(); parent != nullptr; parent = parent->getParentFolderTrack())
            if (parent == &to && isBus (*parent))
                return true;

        return false;
    }
} // namespace

DeviceSlot makeDeviceSlot (te::Plugin& device)
{
    return { getDeviceID (device),       device.getName(),     device.isEnabled(),   hasMix (device),
             device.getLatencySeconds(), isContainer (device), opensWindow (device), isFrozenDevice (device) };
}

//==============================================================================
MixerChannels getMixerChannels (const ProjectContext& context)
{
    MixerChannels channels;

    if (auto* edit = getEdit (context))
        for (auto* track : getPlaylistTracks (*edit))
            (getReturnBus (*track) >= 0 ? channels.returns : channels.tracks).push_back (getTrackID (*track));

    return channels;
}

std::optional<Channel> getChannel (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return {};

    Channel channel;
    channel.id = id;
    channel.name = track->getName();
    channel.colour = getTrackColour (*track, getPlaylistTracks (track->edit).indexOf (track));
    channel.hasEffects = ! phazite::getEffects (*track).isEmpty();
    channel.needsInstrument = phazite::needsInstrument (*track);

    if (auto* folder = dynamic_cast<te::FolderTrack*> (track))
        channel.kind = isBus (*folder) ? ChannelKind::bus : ChannelKind::plainFolder;
    else if (const auto bus = getReturnBus (*track); bus >= 0)
        channel.kind = ChannelKind::returnTrack, channel.senders = getSenderNames (track->edit, bus);

    // A track's sound goes into the nearest bus it is in, or else to the master.
    for (auto* parent = track->getParentFolderTrack(); parent != nullptr; parent = parent->getParentFolderTrack())
    {
        if (isBus (*parent))
        {
            channel.outputBus = parent->getName();
            break;
        }
    }

    return channel;
}

bool feeds (const ProjectContext& context, TrackID from, TrackID to)
{
    auto* source = findTrack (context, from);
    auto* destination = findTrack (context, to);
    return source != nullptr && destination != nullptr && reaches (*source, *destination);
}

TrackID getInspectedChannel (const ProjectContext& context)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return {};

    te::Track* track = getSelectedTracks (getSelection (context)).getFirst();

    if (track == nullptr)
        track = getSelectedAudioTrack (getSelection (context));

    if (track == nullptr)
        track = getLiveMidiTrack (*edit);

    if (track == nullptr)
        track = getPlaylistTracks (*edit).getFirst();

    return track != nullptr ? getTrackID (*track) : TrackID();
}

//==============================================================================
float faderPositionToDecibels (float position)
{
    return te::volumeFaderPositionToDB (position);
}

float decibelsToFaderPosition (float decibels)
{
    return te::decibelsToVolumeFaderPosition (decibels);
}

ParameterHandle getVolume (ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    return track != nullptr ? makeParameterHandle (getVolumeDevice (*track), getVolumeParameterID (*track), "Change Volume")
                            : ParameterHandle();
}

ParameterHandle getPan (ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    auto* fader = track != nullptr ? dynamic_cast<te::VolumeAndPanPlugin*> (getVolumeDevice (*track)) : nullptr;
    return fader != nullptr ? makeParameterHandle (fader, fader->panParam->paramID, "Change Pan") : ParameterHandle();
}

LevelSource getLevel (ProjectContext& context, TrackID id)
{
    if (getEdit (context) == nullptr || ! id.isValid())
        return {};

    // The track is looked up again only once it has gone (its deletion undone brings back another object).
    return makeLevelSource (
        [edit = te::SafeSelectable<te::Edit> (getEdit (context)), id,
         track = te::SafeSelectable<te::Track>()]() mutable -> te::LevelMeasurer*
        {
            if (auto* e = edit.get(); e != nullptr && track.get() == nullptr)
                track = te::findTrackForID (*e, toTracktion (id));

            auto* t = track.get();
            return t != nullptr ? getLevelMeasurer (*t) : nullptr;
        });
}

ParameterHandle getMasterVolume (ProjectContext& context)
{
    auto* edit = getEdit (context);
    const auto fader = edit != nullptr ? edit->getMasterVolumePlugin() : nullptr;
    return fader != nullptr ? makeParameterHandle (fader.get(), fader->volParam->paramID, "Change Master Volume") : ParameterHandle();
}

LevelSource getMasterLevel (ProjectContext& context)
{
    // The playback context, which holds the master's levels, comes and goes as playback is set up again.
    return makeLevelSource (
        [edit = te::SafeSelectable<te::Edit> (getEdit (context))]() -> te::LevelMeasurer*
        {
            auto* e = edit.get();
            auto* playback = e != nullptr ? e->getCurrentPlaybackContext() : nullptr;
            return playback != nullptr ? &playback->masterLevels : nullptr;
        });
}

std::optional<MuteSolo> getMuteSolo (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return {};

    return MuteSolo { track->isMuted (false), track->isSolo (false), isSilencedBySolo (*track) };
}

void toggleMute (ProjectContext& context, TrackID id)
{
    if (auto* track = findTrack (context, id))
        toggleTracksMuted ({ track });
}

void toggleSolo (ProjectContext& context, TrackID id)
{
    if (auto* track = findTrack (context, id))
        toggleTracksSoloed ({ track });
}

void toggleSoloAlone (ProjectContext& context, TrackID id)
{
    if (auto* track = findTrack (context, id))
        phazite::toggleSoloAlone (*track);
}

void setMixedAsBus (ProjectContext& context, TrackID id, bool bus)
{
    if (auto* folder = dynamic_cast<te::FolderTrack*> (findTrack (context, id)))
        phazite::setMixedAsBus (*folder, bus);
}

//==============================================================================
std::optional<DeviceSlot> getInstrument (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    auto* instrument = track != nullptr ? phazite::getInstrument (*track) : nullptr;

    if (instrument == nullptr)
        return {};

    return makeDeviceSlot (*instrument);
}

std::vector<DeviceSlot> getEffects (const ProjectContext& context, TrackID id)
{
    if (! id.isValid())
        return getEdit (context) != nullptr ? makeSlots (getMasterDevices (*getEdit (context))) : std::vector<DeviceSlot>();

    auto* track = findTrack (context, id);
    return track != nullptr ? makeSlots (phazite::getEffects (*track)) : std::vector<DeviceSlot>();
}

bool canHoldInstrument (const ProjectContext& context, TrackID id)
{
    auto* track = dynamic_cast<te::AudioTrack*> (findTrack (context, id));
    return track != nullptr && getReturnBus (*track) < 0;
}

std::vector<ParameterHandle> getKeyControls (ProjectContext& context, DeviceID id, int maximum)
{
    std::vector<ParameterHandle> handles;
    auto* device = findDevice (context, id);

    // A container's are its first macros.
    if (auto* rack = device != nullptr ? getContainerRack (*device) : nullptr)
    {
        for (auto* macro : getContainerMacros (*device))
            if ((int) handles.size() < maximum)
                handles.push_back (makeMacroHandle (*rack, *macro, "Change " + macro->macroName.get()));

        return handles;
    }

    if (device != nullptr)
        for (const auto& parameterID : phazite::getKeyControls (*device, maximum))
            handles.push_back (makeParameterHandle (device, parameterID));

    return handles;
}

MixHandle getMix (ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    return device != nullptr ? makeMixHandle (*device) : MixHandle();
}

void setDeviceOn (ProjectContext& context, DeviceID id, bool on)
{
    if (auto* device = findDevice (context, id))
        phazite::setDeviceOn (*device, on);
}

void moveEffect (ProjectContext& context, DeviceID id, int places)
{
    auto* device = findDevice (context, id);
    auto* edit = getEdit (context);

    if (device == nullptr || edit == nullptr)
        return;

    // Among its container's devices.
    if (auto* container = findContainerOf (*device))
    {
        const auto devices = getContainerDevices (*container);
        const auto target = devices.indexOf (device) + places;

        if (juce::isPositiveAndBelow (target, devices.size()))
        {
            const UndoStep step (*edit, "Move Device");
            moveInContainer (*device, target);
        }

        return;
    }

    // Among the channel's effects, which follow its instrument among all its devices.
    auto* track = device->getOwnerTrack();
    const auto effects = track != nullptr ? phazite::getEffects (*track) : getMasterDevices (*edit);
    const auto all = track != nullptr ? getDevices (*track) : getMasterDevices (*edit);
    const auto target = effects.indexOf (device) + places;

    if (effects.contains (device) && juce::isPositiveAndBelow (target, effects.size()))
        moveDevice (*device, all.indexOf (effects[target]));
}

void removeDevice (ProjectContext& context, DeviceID id)
{
    if (auto* device = findDevice (context, id))
        phazite::removeDevice (*device);
}

void showDeviceWindow (ProjectContext& context, DeviceID id)
{
    if (auto* device = findDevice (context, id); device != nullptr && opensWindow (*device) && ! isFrozenDevice (*device))
        device->showWindowExplicitly();
}

DeviceHandle getDeviceHandle (ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    return device != nullptr ? makeDeviceHandle (*device) : DeviceHandle();
}

std::optional<TrackID> getDeviceChannel (const ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);

    if (device == nullptr)
        return {};

    // A container's devices are on its channel (containers don't nest). The master's devices belong to Tracktion's master
    // track, which the mixer calls the master, by an empty ID.
    if (auto* container = findContainerOf (*device))
        device = container;

    if (getMasterDevices (device->edit).contains (device))
        return TrackID();

    if (auto* track = device->getOwnerTrack())
        return getTrackID (*track);

    return {};
}

std::vector<DeviceType> getDeviceTypes (const ProjectContext& context)
{
    return phazite::getDeviceTypes (getSelection (context).engine);
}

void chooseInstrument (ProjectContext& context, TrackID id, const DeviceType& type)
{
    if (auto* track = dynamic_cast<te::AudioTrack*> (findTrack (context, id)))
        phazite::chooseInstrument (*track, type);
}

void addEffect (ProjectContext& context, TrackID id, const DeviceType& type)
{
    auto* edit = getEdit (context);
    auto* track = findTrack (context, id);

    if (edit != nullptr && (track != nullptr || ! id.isValid()))
        phazite::addEffect (*edit, track, type);
}

TrackID addReturnWith (ProjectContext& context, TrackID sender, const DeviceType& type)
{
    auto* track = findTrack (context, sender);
    auto* added = track != nullptr ? phazite::addReturnWith (*track, type) : nullptr;
    return added != nullptr ? getTrackID (*added) : TrackID();
}

bool addDeviceTo (ProjectContext& context, TrackID id, const DeviceType& type)
{
    auto* edit = getEdit (context);
    auto* track = findTrack (context, id);

    if (edit == nullptr || (track == nullptr && id.isValid()))
        return false;

    return phazite::addDeviceTo (*edit, track, type, &getSelection (context)) != nullptr; // selects a track made for it
}

//==============================================================================
std::vector<Send> getSends (const ProjectContext& context, TrackID id)
{
    std::vector<Send> sends;

    if (auto* track = findTrack (context, id))
    {
        for (auto* send : phazite::getSends (*track))
        {
            const auto* target = findReturnTrack (track->edit, send->getBusNumber());
            sends.push_back ({ getDeviceID (*send), target != nullptr ? target->getName() : send->getBusName(), isPreFader (*send) });
        }
    }

    return sends;
}

ParameterHandle getSendLevel (ProjectContext& context, DeviceID id)
{
    auto* send = dynamic_cast<te::AuxSendPlugin*> (findDevice (context, id));
    return send != nullptr ? makeParameterHandle (send, send->gain->paramID, "Change Send Level") : ParameterHandle();
}

void setPreFader (ProjectContext& context, DeviceID id, bool preFader)
{
    if (auto* send = dynamic_cast<te::AuxSendPlugin*> (findDevice (context, id)))
        phazite::setPreFader (*send, preFader);
}

void removeSend (ProjectContext& context, DeviceID id)
{
    if (auto* send = dynamic_cast<te::AuxSendPlugin*> (findDevice (context, id)))
        phazite::removeSend (*send);
}

std::vector<Return> getReturns (const ProjectContext& context)
{
    std::vector<Return> returns;

    if (auto* edit = getEdit (context))
        for (auto* track : getReturnTracks (*edit))
            returns.push_back ({ getTrackID (*track), track->getName() });

    return returns;
}

void addSendTo (ProjectContext& context, TrackID from, TrackID returnTrack)
{
    auto* track = findTrack (context, from);
    auto* target = findTrack (context, returnTrack);

    if (track != nullptr && target != nullptr && getReturnBus (*target) >= 0)
        phazite::addSendTo (*track, getReturnBus (*target));
}

void addSendToNewReturn (ProjectContext& context, TrackID from)
{
    if (auto* track = findTrack (context, from))
        phazite::addSendToNewReturn (*track);
}

//==============================================================================
namespace
{
    te::AuxSendPlugin* findSendTo (const te::Track& track, int bus)
    {
        for (auto* send : phazite::getSends (track))
            if (send->getBusNumber() == bus)
                return send;

        return nullptr;
    }

    // Tracks and returns send, folders don't; and a return whose sound comes back to the track would make a loop.
    bool canSendTo (te::Track& from, te::AudioTrack& returnTrack)
    {
        return &from != &returnTrack && ! from.isFolderTrack() && ! getTracksFedBy (returnTrack).contains (&from);
    }

    // The parameter of Tracktion's send that sets its level (tracktion_AuxSend.cpp), which the send's knob changes.
    const juce::String sendLevelID ("send level");
} // namespace

std::vector<SendTarget> getSendTargets (const ProjectContext& context, TrackID id)
{
    std::vector<SendTarget> targets;
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return targets;

    for (auto* returnTrack : getReturnTracks (track->edit))
    {
        if (! canSendTo (*track, *returnTrack))
            continue;

        SendTarget target { getTrackID (*returnTrack), returnTrack->getName(), std::nullopt };

        if (auto* send = findSendTo (*track, getReturnBus (*returnTrack)))
            target.send = Send { getDeviceID (*send), returnTrack->getName(), isPreFader (*send) };

        targets.push_back (target);
    }

    return targets;
}

ParameterHandle getSendLevelTo (ProjectContext& context, TrackID from, TrackID returnTrack)
{
    auto* track = findTrack (context, from);
    auto* target = dynamic_cast<te::AudioTrack*> (findTrack (context, returnTrack));
    const auto bus = target != nullptr ? getReturnBus (*target) : -1;

    if (track == nullptr || bus < 0 || ! canSendTo (*track, *target))
        return {};

    // The send goes after the fader, as every new send does.
    const auto locate = [bus] (te::Track& sender) -> te::Plugin* { return findSendTo (sender, bus); };
    const auto create = [bus] (te::Track& sender) -> te::Plugin* { return addSend (sender, bus, false); };
    const auto identity = "send " + juce::String (from.getRaw()) + " " + juce::String (returnTrack.getRaw());
    return makeCreatingHandle (*track, locate, create, identity, sendLevelID, { 0.0f, 1.0f }, "Send to " + target->getName(),
                               "Change Send Level");
}

//==============================================================================
std::vector<OutputChoice> getOutputChoices (const ProjectContext& context, TrackID id)
{
    std::vector<OutputChoice> choices;
    auto* track = findTrack (context, id);

    if (track == nullptr || getReturnBus (*track) >= 0)
        return choices;

    const auto* current = getOutputBus (*track);
    choices.push_back ({ {}, "Master", current == nullptr });

    // The buses, in the playlist's order: not the channel itself, nor a folder inside it.
    for (auto* other : getPlaylistTracks (track->edit))
    {
        auto* folder = dynamic_cast<te::FolderTrack*> (other);

        if (folder != nullptr && folder != track && isBus (*folder) && ! folder->isAChildOf (*track))
            choices.push_back ({ getTrackID (*folder), folder->getName(), folder == current });
    }

    return choices;
}

bool canChooseOutput (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    return track != nullptr && getReturnBus (*track) < 0;
}

void setOutput (ProjectContext& context, TrackID id, TrackID busID)
{
    auto* track = findTrack (context, id);
    auto* bus = busID.isValid() ? dynamic_cast<te::FolderTrack*> (findTrack (context, busID)) : nullptr;

    if (track == nullptr || getReturnBus (*track) >= 0 || (busID.isValid() && bus == nullptr))
        return;

    if (bus != nullptr && (bus == track || ! isBus (*bus) || bus->isAChildOf (*track)))
        return;

    if (getOutputBus (*track) == bus)
        return;

    const UndoStep step (track->edit, "Change Output");

    if (bus != nullptr)
    {
        moveTrackIntoFolder (*track, *bus);
        return;
    }

    // Out of its buses, after the outermost: a plain folder around them keeps it.
    te::FolderTrack* outermost = nullptr;

    for (auto* parent = track->getParentFolderTrack(); parent != nullptr; parent = parent->getParentFolderTrack())
        if (isBus (*parent))
            outermost = parent;

    if (outermost != nullptr)
        track->edit.moveTrack (track, getInsertPointAfter (*outermost));
}

//==============================================================================
bool hasSidechain (const ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    return device != nullptr && phazite::hasSidechain (*device);
}

std::vector<DeviceHandle::SidechainChoice> getSidechainChoices (const ProjectContext& context, DeviceID id)
{
    std::vector<DeviceHandle::SidechainChoice> choices;

    if (auto* device = findDevice (context, id))
        for (auto* source : getSidechainSources (*device))
            choices.push_back ({ getTrackID (*source), source->getName() });

    return choices;
}

TrackID getSidechainSource (const ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    auto* source = device != nullptr ? phazite::getSidechainSource (*device) : nullptr;
    return source != nullptr ? getTrackID (*source) : TrackID();
}

void setSidechainSource (ProjectContext& context, DeviceID id, TrackID sourceID)
{
    auto* device = findDevice (context, id);
    auto* source = sourceID.isValid() ? dynamic_cast<te::AudioTrack*> (findTrack (context, sourceID)) : nullptr;

    if (device != nullptr && (source != nullptr || ! sourceID.isValid()))
        phazite::setSidechainSource (*device, source);
}

//==============================================================================
float getCpuLoad (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return 0.0f;

    auto load = 0.0;

    for (auto* device : track->pluginList)
    {
        load += device->getCpuUsage();

        // A container's devices, which play in its rack.
        if (auto* container = dynamic_cast<te::RackInstance*> (device); container != nullptr && container->type != nullptr)
            for (auto* inside : container->type->getPlugins())
                load += inside->getCpuUsage();
    }

    return (float) juce::jlimit (0.0, 1.0, load);
}

//==============================================================================
std::vector<Connection> getConnections (const ProjectContext& context)
{
    std::vector<Connection> connections;
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return connections;

    for (auto* track : getChannelTracks (*edit))
    {
        const auto id = getTrackID (*track);

        // A plain folder has no sound of its own to send anywhere.
        if (! track->isFolderTrack() || isBus (*track))
        {
            const auto* bus = getOutputBus (*track);
            connections.push_back ({ Connection::Kind::output, id, bus != nullptr ? getTrackID (*bus) : TrackID(), {} });
        }

        for (auto* send : phazite::getSends (*track))
            if (auto* returnTrack = findReturnTrack (*edit, send->getBusNumber()))
                connections.push_back ({ Connection::Kind::send, id, getTrackID (*returnTrack), getDeviceID (*send) });

        for (auto* device : track->pluginList)
            if (auto* source = phazite::getSidechainSource (*device))
                connections.push_back ({ Connection::Kind::sidechain, getTrackID (*source), id, getDeviceID (*device) });
    }

    return connections;
}

//==============================================================================
std::optional<Loudness> getLoudness (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    auto* meter = edit != nullptr ? findLoudnessMeter (*edit) : nullptr;

    if (meter == nullptr)
        return {};

    const auto readings = meter->getReadings();
    const auto valid = [] (bool isValid, float value) { return isValid ? std::optional<float> (value) : std::nullopt; };
    return Loudness { valid (readings.momentaryValid, readings.momentaryLufs), valid (readings.shortTermValid, readings.shortTermLufs),
                      valid (readings.integratedValid, readings.integratedLufs),
                      valid (readings.truePeakDb > te::LoudnessMeter::silenceFloorDb, readings.truePeakDb) };
}

void resetLoudness (ProjectContext& context)
{
    if (auto* edit = getEdit (context))
        if (auto* meter = findLoudnessMeter (*edit))
            meter->resetReadings();
}

//==============================================================================
DelayCompensation getDelayCompensation (const ProjectContext& context)
{
    DelayCompensation compensation;
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return compensation;

    for (auto* track : te::getAllTracks (*edit))
        for (auto* plugin : track->pluginList.getPlugins())
            if (plugin->isEnabled())
                if (const auto latency = plugin->getLatencySeconds(); latency > 0.0)
                    compensation.devices.push_back (
                        { plugin->getName() + " on " + (track->isMasterTrack() ? juce::String ("Master") : track->getName()), latency });

    std::stable_sort (compensation.devices.begin(), compensation.devices.end(),
                      [] (const auto& a, const auto& b) { return a.second > b.second; });

    // What the playback graph waits for, the longest of its paths; the latest device without one.
    if (edit->isLatencyCompensationEnabled())
    {
        const auto rate = edit->engine.getDeviceManager().getSampleRate();

        if (auto* playback = edit->getTransport().getCurrentPlaybackContext(); playback != nullptr && rate > 0.0)
            compensation.seconds = playback->getLatencySamples() / rate;
        else if (! compensation.devices.empty())
            compensation.seconds = compensation.devices.front().second;
    }

    return compensation;
}
} // namespace phazite
