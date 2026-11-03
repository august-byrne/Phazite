// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixerModel.h"
#include "engine/nodes/Channel.h"
#include "internal/Containers.h"
#include "internal/Freeze.h"
#include "internal/LevelSource.h"
#include "internal/MixHandle.h"
#include "internal/Mixer.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"
#include "model/mirror/internal/Mirror.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Tracktion's track or device for one of the song's, for what is still reached through it (docs/tracktion-migration.md,
    // T6: parameters, windows, meters and plug-ins' instances).
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? mirror::findTrack (*edit, id) : nullptr;
    }

    te::Plugin* findDevice (const ProjectContext& context, DeviceID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr ? mirror::findDevice (*edit, id) : nullptr;
    }

    // A track of the playlist's (a track, a folder or a return); and one with sound of its own (not a folder).
    bool isTrack (const Song& song, TrackID id)
    {
        return findTrackState (song, id).hasType (schema::TRACK);
    }

    bool hasSound (const Song& song, TrackID id)
    {
        return isTrack (song, id) && ! isFolder (song, id);
    }

    bool contains (const std::vector<TrackID>& tracks, TrackID track)
    {
        return std::find (tracks.begin(), tracks.end(), track) != tracks.end();
    }

    std::vector<DeviceSlot> makeSlots (const Song& song, const std::vector<DeviceID>& devices)
    {
        std::vector<DeviceSlot> slots;

        for (const auto device : devices)
            slots.push_back (makeDeviceSlot (song, device));

        return slots;
    }

    // The tracks that send to a return.
    juce::StringArray getSenderNames (const Song& song, TrackID returnTrack)
    {
        juce::StringArray names;

        for (const auto track : getTrackIDs (song))
            for (const auto send : getSends (song, track))
                if (getSendTarget (song, send) == returnTrack)
                    names.addIfNotAlreadyThere (getTrackName (song, track));

        return names;
    }

    // A track's send to a return, or none.
    DeviceID findSendTo (const Song& song, TrackID from, TrackID returnTrack)
    {
        for (const auto send : getSends (song, from))
            if (getSendTarget (song, send) == returnTrack)
                return send;

        return {};
    }

    // Whether one channel's sound reaches another's: through a send to a return, or by being in a bus folder.
    bool reaches (const Song& song, TrackID from, TrackID to)
    {
        if (isReturnTrack (song, to) && findSendTo (song, from, to).isValid())
            return true;

        for (auto parent = getParentFolder (song, from); parent.isValid(); parent = getParentFolder (song, parent))
            if (parent == to && isBus (song, parent))
                return true;

        return false;
    }

    // Tracks and returns send, folders don't; and a return whose sound comes back to the track would make a loop.
    bool canSendTo (const Song& song, TrackID from, TrackID returnTrack)
    {
        return from != returnTrack && hasSound (song, from) && isReturnTrack (song, returnTrack)
               && ! contains (getTracksFedBy (song, returnTrack), from);
    }

    // The parameter of Tracktion's send that sets its level (tracktion_AuxSend.cpp), which the send's knob changes.
    const juce::String sendLevelID ("send level");
} // namespace

DeviceSlot makeDeviceSlot (const Song& song, DeviceID id)
{
    auto* edit = findEdit (song);
    auto* device = edit != nullptr ? mirror::findDevice (*edit, id) : nullptr;
    const auto on = isDeviceOn (song, id);

    // How late its sound comes out, as Phazite's engine plays it while it plays the device's project, in seconds; or as
    // Tracktion knows it otherwise. While Low Latency is on, one that looks ahead further than its limit is bypassed
    // (EngineSetup.h).
    auto latency = device != nullptr && on ? device->getLatencySeconds() : 0.0;

    if (auto* session = edit != nullptr ? findPlayingSession (*edit) : nullptr)
    {
        auto* node = session->getPlayer().findDevice ((std::int64_t) id.getRaw());
        latency = node != nullptr && node->isOn() ? node->getRunner().getLatencySamples() / session->getEngine().getSampleRate() : 0.0;
        latency = isLowLatency (edit->engine) && latency > lowLatencyLimitSeconds ? 0.0 : latency;
    }

    return { .id = id,
             .name = getDeviceName (song, id),
             .isOn = on,
             .hasMix = hasMix (song, id),
             .latencySeconds = latency,
             .isContainer = isContainer (song, id),
             .opensWindow = opensWindow (song, id),
             .popsOut = popsOut (song, id),
             .windowOpen = popsOut (song, id) && device != nullptr && isWindowOpen (*device),
             .frozen = device != nullptr && isFrozenDevice (*device),
             .crashed = device != nullptr && hasCrashed (*device),
             .hasSidechain = hasSidechain (song, id),
             .sidechain = describeSidechainSource (song, id) };
}

//==============================================================================
MixerChannels getMixerChannels (const ProjectContext& context)
{
    MixerChannels channels;

    if (auto* song = findSong (context))
        for (const auto track : getTrackIDs (*song))
            (isReturnTrack (*song, track) ? channels.returns : channels.tracks).push_back (track);

    return channels;
}

std::optional<Channel> getChannel (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isTrack (*song, id))
        return {};

    const auto tracks = getTrackIDs (*song);
    const auto row = (int) std::distance (tracks.begin(), std::find (tracks.begin(), tracks.end(), id));

    Channel channel;
    channel.id = id;
    channel.name = getTrackName (*song, id);
    channel.colour = getTrackColour (*song, id, row);
    channel.hasEffects = ! getEffects (*song, id).empty();
    channel.needsInstrument = needsInstrument (*song, id);

    if (isFolder (*song, id))
        channel.kind = isBus (*song, id) ? ChannelKind::bus : ChannelKind::plainFolder;
    else if (isReturnTrack (*song, id))
        channel.kind = ChannelKind::returnTrack, channel.senders = getSenderNames (*song, id);

    // A track's sound goes into the nearest bus it is in, or else to the master.
    if (const auto bus = getOutputBus (*song, id); bus.isValid())
        channel.outputBus = getTrackName (*song, bus);

    return channel;
}

bool feeds (const ProjectContext& context, TrackID from, TrackID to)
{
    auto* song = findSong (context);
    return song != nullptr && isTrack (*song, from) && isTrack (*song, to) && reaches (*song, from, to);
}

TrackID getInspectedChannel (const ProjectContext& context)
{
    auto* song = findSong (context);
    auto* edit = getEdit (context);

    if (song == nullptr || edit == nullptr)
        return {};

    if (const auto selected = getSelectedTracks (getSelection (context)); ! selected.empty())
        return selected.front();

    te::Track* track = getSelectedAudioTrack (getSelection (context));

    if (track == nullptr)
        track = getLiveMidiTrack (*edit);

    if (track != nullptr)
        return getTrackID (*track);

    const auto tracks = getTrackIDs (*song);
    return tracks.empty() ? TrackID() : tracks.front();
}

//==============================================================================
float faderPositionToDecibels (float position)
{
    return engine::faderPositionToDecibels (position);
}

float decibelsToFaderPosition (float decibels)
{
    return engine::decibelsToFaderPosition (decibels);
}

ParameterHandle getVolume (ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isTrack (*song, id))
        return {};

    auto* fader = findDevice (context, getFader (*song, id));
    return fader != nullptr ? makeParameterHandle (fader, getVolumeParameterID (*song, id), "Change Volume") : ParameterHandle();
}

ParameterHandle getPan (ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isTrack (*song, id) || getPanParameterID (*song, id).isEmpty())
        return {};

    auto* fader = findDevice (context, getFader (*song, id));
    return fader != nullptr ? makeParameterHandle (fader, getPanParameterID (*song, id), "Change Pan") : ParameterHandle();
}

namespace
{
    // A channel's meter (a track's by its ID, the master's by 0) in Phazite's own engine, while it plays the project.
    engine::LevelMeter* findOwnMeter (ProjectContext& context, std::int64_t id)
    {
        auto* song = findSong (context);
        auto* edit = getEdit (context);

        // A track's while it is the song's (the engine may play one deleted a moment ago), the master's always.
        if (auto* session = edit != nullptr ? findPlayingSession (*edit) : nullptr)
            if (id == 0 || (song != nullptr && findTrackState (*song, TrackID::fromRaw ((std::uint64_t) id)).isValid()))
                if (auto* strip = session->getPlayer().findStrip (id))
                    return &strip->getChannel().getMeter();

        return nullptr;
    }
} // namespace

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
                track = mirror::findTrack (*e, id);

            auto* t = track.get();
            return t != nullptr ? getLevelMeasurer (*t) : nullptr;
        },
        [&context, id] { return findOwnMeter (context, (std::int64_t) id.getRaw()); });
}

ParameterHandle getMasterVolume (ProjectContext& context)
{
    auto* song = findSong (context);
    auto* fader = song != nullptr ? findDevice (context, getFader (*song, {})) : nullptr;
    return fader != nullptr ? makeParameterHandle (fader, getVolumeParameterID (*song, {}), "Change Master Volume") : ParameterHandle();
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
        },
        [&context] { return findOwnMeter (context, 0); });
}

std::optional<MuteSolo> getMuteSolo (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isTrack (*song, id))
        return {};

    return MuteSolo { isTrackMuted (*song, id, false), isTrackSoloed (*song, id), isSilencedBySolo (*song, id) };
}

void toggleMute (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context))
        toggleTracksMuted (*song, { id });
}

void toggleSolo (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context))
        toggleTracksSoloed (*song, { id });
}

void toggleSoloAlone (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context); song != nullptr && isTrack (*song, id))
        toggleSoloAlone (*song, id);
}

void setMixedAsBus (ProjectContext& context, TrackID id, bool bus)
{
    if (auto* song = findSong (context))
        setMixedAsBus (*song, id, bus);
}

//==============================================================================
std::optional<DeviceSlot> getInstrument (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    const auto instrument = song != nullptr && isTrack (*song, id) ? getInstrument (*song, id) : DeviceID();

    if (! instrument.isValid())
        return {};

    return makeDeviceSlot (*song, instrument);
}

std::vector<DeviceSlot> getEffects (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr ? makeSlots (*song, getEffects (*song, id)) : std::vector<DeviceSlot>();
}

bool canHoldInstrument (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && hasSound (*song, id) && ! isReturnTrack (*song, id);
}

std::vector<ParameterHandle> getKeyControls (ProjectContext& context, DeviceID id, int maximum)
{
    std::vector<ParameterHandle> handles;
    auto* device = findDevice (context, id);

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
    auto* song = findSong (context);

    if (song == nullptr || ! findDeviceState (*song, id).isValid())
        return;

    // An instrument stays on: its track's switch mutes it (the owner, 2026-11-01: with one instrument a track, its own switch
    // was a second way to silence the track).
    if (! on && getInstrument (*song, getDeviceChannel (*song, id)) == id)
        return;

    setDeviceOn (*song, id, on);
}

void moveEffect (ProjectContext& context, DeviceID id, int places)
{
    auto* song = findSong (context);

    if (song == nullptr || ! findDeviceState (*song, id).isValid())
        return;

    const auto indexOf = [] (const std::vector<DeviceID>& devices, DeviceID device)
    { return (int) std::distance (devices.begin(), std::find (devices.begin(), devices.end(), device)); };

    // Among its container's devices.
    if (const auto container = findContainerOf (*song, id); container.isValid())
    {
        const auto devices = getContainerDevices (*song, container);
        const auto target = indexOf (devices, id) + places;

        if (juce::isPositiveAndBelow (target, (int) devices.size()))
        {
            const UndoStep step (*song, "Move Device");
            moveInContainer (*song, id, target);
        }

        return;
    }

    // Among the channel's effects, which follow its instrument among all its devices.
    const auto channel = getDeviceChannel (*song, id);
    const auto effects = getEffects (*song, channel);
    const auto all = getDevices (*song, channel);
    const auto target = indexOf (effects, id) + places;

    if (indexOf (effects, id) < (int) effects.size() && juce::isPositiveAndBelow (target, (int) effects.size()))
        moveDevice (*song, id, indexOf (all, effects[(size_t) target]));
}

void removeDevice (ProjectContext& context, DeviceID id)
{
    if (auto* song = findSong (context))
        removeDevice (*song, id);
}

void replaceEffect (ProjectContext& context, DeviceID id, const DeviceType& type)
{
    auto* song = findSong (context);

    if (song == nullptr || type.isInstrument || ! findDeviceState (*song, id).isValid() || isInstrumentState (findDeviceState (*song, id)))
        return;

    const UndoStep step (*song, "Replace " + getDeviceName (*song, id) + " with " + type.name);
    const auto replacement = makeDeviceState (*song, type);
    const auto indexOf = [] (const std::vector<DeviceID>& devices, DeviceID device)
    { return (int) std::distance (devices.begin(), std::find (devices.begin(), devices.end(), device)); };

    // The new one where the old one is, then the old one out (a bus keeps a device all along).
    if (const auto container = findContainerOf (*song, id); container.isValid())
        insertIntoContainer (*song, container, replacement, indexOf (getContainerDevices (*song, container), id));
    else if (const auto channel = getDeviceChannel (*song, id); channel.isValid())
        insertDeviceState (*song, channel, replacement, indexOf (getDevices (*song, channel), id));

    removeDevice (*song, id);
}

std::optional<juce::String> reloadDevice (ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);

    if (device == nullptr || ! hasCrashed (*device))
        return {};

    if (juce::String error; ! reload (*device, error))
        return error.isNotEmpty() ? error : juce::String ("It didn't load.");

    return {};
}

void showDeviceWindow (ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    auto* device = findDevice (context, id);

    if (song != nullptr && device != nullptr && (opensWindow (*song, id) || popsOut (*song, id)) && ! isFrozenDevice (*device))
        device->showWindowExplicitly();
}

bool isDeviceWindowOpen (const ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    return device != nullptr && isWindowOpen (*device);
}

DeviceHandle getDeviceHandle (ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    return device != nullptr ? makeDeviceHandle (*device) : DeviceHandle();
}

std::optional<TrackID> getDeviceChannel (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);

    if (song == nullptr || ! findDeviceState (*song, id).isValid())
        return {};

    // A container's devices are on its channel (containers don't nest); the master's, which the mixer calls the master, by
    // an empty ID.
    const auto channel = getDeviceChannel (*song, id);
    return channel == getMasterID (*song) ? TrackID() : channel;
}

std::vector<DeviceType> getDeviceTypes (const ProjectContext& context)
{
    return phazite::getDeviceTypes (getSelection (context).engine);
}

void chooseInstrument (ProjectContext& context, TrackID id, const DeviceType& type)
{
    if (auto* song = findSong (context); song != nullptr && canHoldInstrument (context, id))
        chooseInstrument (*song, id, type);
}

void addEffect (ProjectContext& context, TrackID id, const DeviceType& type)
{
    if (auto* song = findSong (context); song != nullptr && (! id.isValid() || isTrack (*song, id)))
        addEffect (*song, id, type);
}

TrackID addReturnWith (ProjectContext& context, TrackID sender, const DeviceType& type)
{
    auto* song = findSong (context);
    return song != nullptr && hasSound (*song, sender) ? addReturnWith (*song, sender, type) : TrackID();
}

bool addDeviceTo (ProjectContext& context, TrackID id, const DeviceType& type)
{
    auto* song = findSong (context);

    if (song == nullptr || (id.isValid() && ! isTrack (*song, id)))
        return false;

    return addDeviceTo (*song, id, type, &getSelection (context)).isValid(); // selects a track made for it
}

//==============================================================================
std::vector<Send> getSends (const ProjectContext& context, TrackID id)
{
    std::vector<Send> sends;

    if (auto* song = findSong (context))
        for (const auto send : getSends (*song, id))
            sends.push_back ({ send, getTrackName (*song, getSendTarget (*song, send)), isPreFader (*song, send) });

    return sends;
}

ParameterHandle getSendLevel (ProjectContext& context, DeviceID id)
{
    auto* send = dynamic_cast<te::AuxSendPlugin*> (findDevice (context, id));
    return send != nullptr ? makeParameterHandle (send, send->gain->paramID, "Change Send Level") : ParameterHandle();
}

void setPreFader (ProjectContext& context, DeviceID id, bool preFader)
{
    if (auto* song = findSong (context))
        setPreFader (*song, id, preFader);
}

void removeSend (ProjectContext& context, DeviceID id)
{
    if (auto* song = findSong (context))
        removeSend (*song, id);
}

std::vector<Return> getReturns (const ProjectContext& context)
{
    std::vector<Return> returns;

    if (auto* song = findSong (context))
        for (const auto track : getReturnTracks (*song))
            returns.push_back ({ track, getTrackName (*song, track) });

    return returns;
}

void addSendTo (ProjectContext& context, TrackID from, TrackID returnTrack)
{
    if (auto* song = findSong (context); song != nullptr && isReturnTrack (*song, returnTrack))
        addSendTo (*song, from, returnTrack);
}

void addSendToNewReturn (ProjectContext& context, TrackID from)
{
    if (auto* song = findSong (context); song != nullptr && hasSound (*song, from))
        addSendToNewReturn (*song, from);
}

std::vector<SendTarget> getSendTargets (const ProjectContext& context, TrackID id)
{
    std::vector<SendTarget> targets;
    auto* song = findSong (context);

    if (song == nullptr || ! isTrack (*song, id))
        return targets;

    for (const auto returnTrack : getReturnTracks (*song))
    {
        if (! canSendTo (*song, id, returnTrack))
            continue;

        SendTarget target { returnTrack, getTrackName (*song, returnTrack), std::nullopt };

        if (const auto send = findSendTo (*song, id, returnTrack); send.isValid())
            target.send = Send { send, getTrackName (*song, returnTrack), isPreFader (*song, send) };

        targets.push_back (target);
    }

    return targets;
}

ParameterHandle getSendLevelTo (ProjectContext& context, TrackID from, TrackID returnTrack)
{
    auto* song = findSong (context);
    auto* track = findTrack (context, from);

    if (song == nullptr || track == nullptr || ! canSendTo (*song, from, returnTrack))
        return {};

    // The send goes after the fader, as every new send does.
    const auto locate = [returnTrack] (te::Track& sender) -> te::Plugin*
    { return mirror::findDevice (sender.edit, findSendTo (getSong (sender.edit), getTrackID (sender), returnTrack)); };
    const auto create = [returnTrack] (te::Track& sender) -> te::Plugin*
    { return mirror::findDevice (sender.edit, addSend (getSong (sender.edit), getTrackID (sender), returnTrack, false)); };
    const auto identity = "send " + juce::String (from.getRaw()) + " " + juce::String (returnTrack.getRaw());
    return makeCreatingHandle (*track, locate, create, identity, sendLevelID, { 0.0f, 1.0f },
                               "Send to " + getTrackName (*song, returnTrack), "Change Send Level");
}

//==============================================================================
std::vector<OutputChoice> getOutputChoices (const ProjectContext& context, TrackID id)
{
    std::vector<OutputChoice> choices;
    auto* song = findSong (context);

    if (song == nullptr || ! isTrack (*song, id) || isReturnTrack (*song, id))
        return choices;

    const auto current = getOutputBus (*song, id);
    choices.push_back ({ {}, "Master", ! current.isValid() });

    // The buses, in the playlist's order: not the channel itself, nor a folder inside it.
    for (const auto other : getTrackIDs (*song))
        if (other != id && isFolder (*song, other) && isBus (*song, other)
            && ! findTrackState (*song, other).isAChildOf (findTrackState (*song, id)))
            choices.push_back ({ other, getTrackName (*song, other), other == current });

    return choices;
}

bool canChooseOutput (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && isTrack (*song, id) && ! isReturnTrack (*song, id);
}

void setOutput (ProjectContext& context, TrackID id, TrackID bus)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canChooseOutput (context, id))
        return;

    if (bus.isValid()
        && (bus == id || ! isFolder (*song, bus) || ! isBus (*song, bus)
            || findTrackState (*song, bus).isAChildOf (findTrackState (*song, id))))
        return;

    if (getOutputBus (*song, id) == bus)
        return;

    const UndoStep step (*song, "Change Output");

    if (bus.isValid())
    {
        moveTrackIntoFolder (*song, id, bus);
        return;
    }

    // Out of its buses, after the outermost: a plain folder around them keeps it.
    TrackID outermost;

    for (auto parent = getParentFolder (*song, id); parent.isValid(); parent = getParentFolder (*song, parent))
        if (isBus (*song, parent))
            outermost = parent;

    if (outermost.isValid())
        moveTracksAfter (*song, { id }, outermost);
}

//==============================================================================
bool hasSidechain (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr && hasSidechain (*song, id);
}

std::vector<SidechainChoice> getSidechainChoices (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr ? describeSidechainChoices (*song, id) : std::vector<SidechainChoice>();
}

TrackID getSidechainSource (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr ? getSidechainSource (*song, id) : TrackID();
}

void setSidechainSource (ProjectContext& context, DeviceID id, TrackID source)
{
    if (auto* song = findSong (context))
        setSidechainSource (*song, id, source);
}

std::vector<SidechainChoice> getDuckUnderChoices (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && hasSound (*song, id) ? describeTracksToListenTo (*song, id) : std::vector<SidechainChoice>();
}

DeviceID duckUnder (ProjectContext& context, TrackID ducked, TrackID key)
{
    auto* song = findSong (context);
    return song != nullptr && hasSound (*song, ducked) && hasSound (*song, key) ? duckUnder (*song, ducked, key) : DeviceID();
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
    auto* song = findSong (context);

    if (song == nullptr)
        return connections;

    for (const auto track : getChannelTracks (*song))
    {
        // A plain folder has no sound of its own to send anywhere.
        if (! isFolder (*song, track) || isBus (*song, track))
            connections.push_back ({ Connection::Kind::output, track, getOutputBus (*song, track), {} });

        for (const auto send : getSends (*song, track))
            if (const auto target = getSendTarget (*song, send); isReturnTrack (*song, target))
                connections.push_back ({ Connection::Kind::send, track, target, send });

        for (const auto device : getDevices (*song, track))
            if (const auto source = getSidechainSource (*song, device); source.isValid())
                connections.push_back ({ Connection::Kind::sidechain, source, track, device });
    }

    return connections;
}

//==============================================================================
std::optional<Loudness> getLoudness (const ProjectContext& context)
{
    auto* edit = getEdit (context);

    // Phazite's engine's own meter, while it plays the project.
    if (auto* session = edit != nullptr ? findPlayingSession (*edit) : nullptr)
    {
        const auto readings = session->getEngine().getLoudnessMeter().getReadings();
        return Loudness { readings.momentary, readings.shortTerm, readings.integrated, readings.truePeak };
    }

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
    {
        if (auto* session = findPlayingSession (*edit))
            session->getEngine().getLoudnessMeter().reset();

        if (auto* meter = findLoudnessMeter (*edit))
            meter->resetReadings();
    }
}

//==============================================================================
DelayCompensation getDelayCompensation (const ProjectContext& context)
{
    DelayCompensation compensation;
    auto* song = findSong (context);
    auto* edit = getEdit (context);

    if (song == nullptr || edit == nullptr)
        return compensation;

    // Each device's, as Phazite's engine plays it, while it plays the project: the master's first, then the channels'.
    std::vector<TrackID> channels { TrackID() };

    for (const auto track : getChannelTracks (*song))
        channels.push_back (track);

    for (const auto channel : channels)
        for (const auto device : getDevices (*song, channel))
            if (const auto latency = makeDeviceSlot (*song, device).latencySeconds; latency > 0.0)
                compensation.devices.push_back ({ getDeviceName (*song, device) + " on "
                                                      + (channel.isValid() ? getTrackName (*song, channel) : juce::String ("Master")),
                                                  latency });

    std::stable_sort (compensation.devices.begin(), compensation.devices.end(),
                      [] (const auto& a, const auto& b) { return a.second > b.second; });

    // What the engine waits for, the longest of its paths; the latest device while it plays nothing.
    if (edit->isLatencyCompensationEnabled())
    {
        auto* session = findPlayingSession (*edit);
        const auto* plan = session != nullptr ? session->getEngine().getPlan() : nullptr;

        if (plan != nullptr)
            compensation.seconds = engine::Engine::getOutputLatency (*plan) / session->getEngine().getSampleRate();
        else if (! compensation.devices.empty())
            compensation.seconds = compensation.devices.front().second;
    }

    return compensation;
}
} // namespace phazite
