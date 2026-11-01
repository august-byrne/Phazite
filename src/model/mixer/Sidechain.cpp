// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sidechain.h"
#include "devices/dynamics/Compressor.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mirror/internal/Mirror.h"

namespace phazite
{
namespace
{
    bool listensToAny (const te::Plugin& plugin, const juce::Array<te::Track*>& tracks)
    {
        const auto source = plugin.getSidechainSourceID();

        if (source.isValid())
            for (auto* track : tracks)
                if (track->itemID == source)
                    return true;

        return false;
    }

    // Whether a track's sound comes from any of these tracks: a bus mixes the tracks inside it, a return takes in what
    // is sent to its bus, and a device can listen to a sidechain. (Returns and sends are read from their aux plug-ins,
    // as Mixer.h makes them: Mixer.h depends on this file, through deleting tracks.)
    bool isFedBy (const te::Track& track, const juce::Array<te::Track*>& feeding)
    {
        if (const auto* folder = dynamic_cast<const te::FolderTrack*> (&track))
            for (auto* inside : folder->getAllSubTracks (false))
                if (feeding.contains (inside))
                    return true;

        for (auto* input : track.pluginList.getPluginsOfType<te::AuxReturnPlugin>())
            for (auto* sender : feeding)
                for (auto* send : sender->pluginList.getPluginsOfType<te::AuxSendPlugin>())
                    if (send->getBusNumber() == input->busNumber.get())
                        return true;

        for (auto* plugin : track.pluginList)
            if (listensToAny (*plugin, feeding))
                return true;

        return false;
    }

    SidechainChoice describe (te::AudioTrack& track)
    {
        return { getTrackID (track), getTrackName (track), getTrackColour (track, getPlaylistTracks (track.edit).indexOf (&track)) };
    }

    bool playsDrumSampler (te::AudioTrack& track)
    {
        for (auto* plugin : track.pluginList)
            if (isDrumSampler (*plugin))
                return true;

        return false;
    }

    // A compressor still as it was made starts ducking, joining the caller's step (setSidechainSource()).
    void startDucking (te::Plugin& plugin, te::AudioTrack& source)
    {
        using devices::Compressor;
        auto* device = dynamic_cast<DevicePlugin*> (&plugin);

        if (device == nullptr || device->getDeviceInfo().typeID != Compressor::typeID)
            return;

        for (const auto& parameter : Compressor::info.parameters)
            if (auto* value = device->findParameter (parameter.id);
                value == nullptr || std::abs (value->getCurrentValue() - parameter.defaultValue) > 1.0e-4f)
                return;

        const auto set = [device] (std::string_view id, float value)
        {
            if (auto* parameter = device->findParameter (id))
                setDeviceValue (*parameter, value);
        };

        // Its curve: 8:1 over -30 dB, its last point in the places the others leave.
        namespace curves = devices::curves;
        const auto points = curves::makeCompression (-30.0f, 8.0f);
        set (Compressor::info.parameters[(size_t) Compressor::points].id, (float) points.size());

        for (int point = 0; point < curves::maxPoints; ++point)
            for (int field = 0; field < curves::pointFields; ++field)
                set (Compressor::info
                         .parameters[(size_t) (Compressor::points + curves::getPointParameter (point, (curves::PointField) field))]
                         .id,
                     points[(size_t) std::min (point, (int) points.size() - 1)][(size_t) field]);

        set ("attack", 1.0f);
        set ("release", 150.0f);

        if (playsDrumSampler (source))
            set ("keyHighCut", 150.0f);
    }
} // namespace

juce::Array<te::Track*> getTracksFedBy (te::Track& track)
{
    juce::Array<te::Track*> fed { &track };
    const auto all = te::getAllTracks (track.edit);

    for (auto grew = true; grew;)
    {
        grew = false;

        for (auto* other : all)
        {
            if (! fed.contains (other) && isFedBy (*other, fed))
            {
                fed.add (other);
                grew = true;
            }
        }
    }

    return fed;
}

bool hasSidechain (te::Plugin& plugin)
{
    // A container wires only the sound passing through its devices (Containers.h).
    return plugin.canSidechain() && ! plugin.isInRack();
}

juce::Array<te::AudioTrack*> getSidechainSources (te::Plugin& plugin)
{
    juce::Array<te::AudioTrack*> sources;

    if (! hasSidechain (plugin))
        return sources;

    // A device on the master, which no track's sound comes from, can listen to any track.
    juce::Array<te::Track*> fed;

    if (auto* owner = plugin.getOwnerTrack())
        fed = getTracksFedBy (*owner);

    for (auto* track : te::getAudioTracks (plugin.edit))
        if (! fed.contains (track))
            sources.add (track);

    return sources;
}

te::AudioTrack* getSidechainSource (te::Plugin& plugin)
{
    const auto source = plugin.getSidechainSourceID();
    return source.isValid() ? dynamic_cast<te::AudioTrack*> (te::findTrackForID (plugin.edit, source)) : nullptr;
}

SidechainChoice describeSidechainSource (te::Plugin& plugin)
{
    auto* source = getSidechainSource (plugin);
    return source != nullptr ? describe (*source) : SidechainChoice();
}

std::vector<SidechainChoice> describeTracksToListenTo (te::Edit& edit, te::Track* listener)
{
    std::vector<SidechainChoice> choices;
    const auto fed = listener != nullptr ? getTracksFedBy (*listener) : juce::Array<te::Track*>();

    for (auto* track : te::getAudioTracks (edit))
    {
        if (track == listener)
            continue;

        auto choice = describe (*track);

        if (fed.contains (track))
            choice.whyNot = "its sound comes from " + getTrackName (*listener);

        choices.push_back (choice);
    }

    return choices;
}

std::vector<SidechainChoice> describeSidechainChoices (te::Plugin& plugin)
{
    if (! hasSidechain (plugin))
        return {};

    return describeTracksToListenTo (plugin.edit, plugin.getOwnerTrack());
}

void setSidechainSource (te::Plugin& plugin, te::AudioTrack* source)
{
    if (source == getSidechainSource (plugin) || (source != nullptr && ! getSidechainSources (plugin).contains (source)))
        return;

    // The song's (Schema.h: deviceSidechain), which the mirror plays.
    auto state = findDeviceState (plugin.edit, getDeviceID (plugin));

    if (! state.isValid())
        return;

    const UndoStep step (plugin.edit, source != nullptr ? "Sidechain from " + getTrackName (*source) : juce::String ("Turn Sidechain Off"));
    auto* undo = &getSongUndoManager (plugin.edit);

    if (source != nullptr)
        state.setProperty (schema::deviceSidechain, (juce::int64) getTrackID (*source).getRaw(), undo);
    else
        state.removeProperty (schema::deviceSidechain, undo);

    mirror::reconcileDevices (plugin.edit);

    if (source != nullptr)
        startDucking (plugin, *source);
}

te::Plugin* duckUnder (te::AudioTrack& ducked, te::AudioTrack& key)
{
    if (&ducked == &key || getTracksFedBy (ducked).contains (&key))
        return nullptr;

    const UndoStep step (ducked.edit, "Duck " + getTrackName (ducked) + " Under " + getTrackName (key));
    auto* compressor = insertDevice (ducked, ducked.edit.getPluginCache().createNewPlugin (devices::Compressor::typeID, {}));

    if (compressor != nullptr)
        setSidechainSource (*compressor, &key);

    return compressor;
}

void forgetSidechainSources (const juce::Array<te::Track*>& tracks)
{
    if (tracks.isEmpty())
        return;

    juce::Array<juce::int64> going;

    for (auto* track : tracks)
    {
        going.addIfNotAlreadyThere ((juce::int64) getTrackID (*track).getRaw());

        for (auto* inside : track->getAllSubTracks (true))
            going.addIfNotAlreadyThere ((juce::int64) getTrackID (*inside).getRaw());
    }

    // The song's devices that listen to them listen to none, which the mirror plays.
    auto& edit = tracks.getFirst()->edit;
    auto chains = getTrackStates (edit);
    chains.push_back (findChainState (edit, nullptr));

    for (const auto& chain : chains)
        for (auto device : getDeviceStates (chain))
            if (going.contains ((juce::int64) device[schema::deviceSidechain]))
                device.removeProperty (schema::deviceSidechain, &getSongUndoManager (edit));

    mirror::reconcileDevices (edit);
}
} // namespace phazite
