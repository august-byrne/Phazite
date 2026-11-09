// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sidechain.h"
#include "devices/dynamics/Compressor.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace phazite
{
namespace
{
    bool contains (const std::vector<TrackID>& tracks, TrackID track)
    {
        return std::find (tracks.begin(), tracks.end(), track) != tracks.end();
    }

    // The tracks with sound of their own, in the song's order: tracks and returns, not folders.
    std::vector<TrackID> getSoundTracks (const Song& song)
    {
        std::vector<TrackID> tracks;

        for (const auto& state : getTrackStates (song))
            if (! isFolderState (state))
                tracks.push_back (getTrackID (state));

        return tracks;
    }

    // Whether a track's sound comes from any of these tracks: a folder mixes the tracks inside it, a return takes in what
    // is sent to it, and a device can listen to a sidechain.
    bool isFedBy (const Song& song, const juce::ValueTree& state, const std::vector<TrackID>& feeding)
    {
        const auto track = getTrackID (state);

        if (isFolderState (state))
            for (const auto inside : getTracksInFolder (song, track))
                if (contains (feeding, inside))
                    return true;

        if (isReturnState (state))
            for (const auto sender : feeding)
                for (const auto send : getSends (song, sender))
                    if (getSendTarget (song, send) == track)
                        return true;

        for (const auto& device : getDeviceStates (state))
            if (contains (feeding, TrackID::fromRaw ((std::uint64_t) (juce::int64) device[schema::deviceSidechain])))
                return true;

        return false;
    }

    SidechainChoice describe (const Song& song, TrackID track)
    {
        const auto tracks = getTrackIDs (song);
        const auto row = (int) std::distance (tracks.begin(), std::find (tracks.begin(), tracks.end(), track));
        return { track, getTrackName (song, track), getTrackColour (song, track, row) };
    }

    bool playsDrumSampler (const Song& song, TrackID track)
    {
        const auto devices = getDeviceStates (findTrackState (song, track));
        return std::any_of (devices.begin(), devices.end(), [] (const juce::ValueTree& device) { return isDrumSampler (device); });
    }

    // A compressor still as it was made starts ducking, joining the caller's step (setSidechainSource()): its values set
    // by hand, which the song holds.
    void startDucking (Song& song, DeviceID id, TrackID source)
    {
        using devices::Compressor;
        const auto state = findDeviceState (song, id);

        if (state[schema::deviceType].toString() != juce::String (Compressor::typeID))
            return;

        for (const auto& parameter : Compressor::info.parameters)
            if (std::abs (readValueSetByHand (state, parameter) - parameter.defaultValue) > 1.0e-4f)
                return;

        auto* undo = &song.getUndoManager();
        const auto setValue = [&] (std::string_view parameter, float value)
        {
            if (const auto index = devices::findParameter (Compressor::info, parameter); index >= 0)
                keepValueSetByHand (state, Compressor::info.parameters[(size_t) index], value, undo);
        };

        // Its curve: 8:1 over -30 dB, its last point in the places the others leave.
        namespace curves = devices::curves;
        const auto points = curves::makeCompression (-30.0f, 8.0f);
        setValue (Compressor::info.parameters[(size_t) Compressor::points].id, (float) points.size());

        for (int point = 0; point < curves::maxPoints; ++point)
            for (int field = 0; field < curves::pointFields; ++field)
                setValue (Compressor::info
                              .parameters[(size_t) (Compressor::points + curves::getPointParameter (point, (curves::PointField) field))]
                              .id,
                          points[(size_t) std::min (point, (int) points.size() - 1)][(size_t) field]);

        setValue ("attack", 1.0f);
        setValue ("release", 150.0f);

        if (playsDrumSampler (song, source))
            setValue ("keyHighCut", 150.0f);
    }
} // namespace

std::vector<TrackID> getTracksFedBy (const Song& song, TrackID track)
{
    std::vector<TrackID> fed { track };
    const auto all = getTrackStates (song);

    for (auto grew = true; grew;)
    {
        grew = false;

        for (const auto& other : all)
        {
            if (! contains (fed, getTrackID (other)) && isFedBy (song, other, fed))
            {
                fed.push_back (getTrackID (other));
                grew = true;
            }
        }
    }

    return fed;
}

bool hasSidechain (const Song& song, DeviceID id)
{
    const auto state = findDeviceState (song, id);

    // A container wires only the sound passing through its devices (Containers.h).
    if (! state.hasType (schema::DEVICE) || getOwningContainer (state).isValid())
        return false;

    if (const auto* own = findOwnDevice (state[schema::deviceType].toString().toStdString()))
        return own->hasSidechain;

    // A plug-in's inputs, as its helper tells them while it plays: a sidechain's after its main ones.
    auto* instance = findPluginInstance (song, id);
    return instance != nullptr && instance->getRemote().getInfo().hasSidechain;
}

std::vector<TrackID> getSidechainSources (const Song& song, DeviceID id)
{
    std::vector<TrackID> sources;

    if (! hasSidechain (song, id))
        return sources;

    // A device on the master, which no track's sound comes from, can listen to any track.
    const auto channel = getDeviceChannel (song, id);
    const auto fed = findTrackState (song, channel).hasType (schema::TRACK) ? getTracksFedBy (song, channel) : std::vector<TrackID>();

    for (const auto track : getSoundTracks (song))
        if (! contains (fed, track))
            sources.push_back (track);

    return sources;
}

TrackID getSidechainSource (const Song& song, DeviceID id)
{
    const auto source = TrackID::fromRaw ((std::uint64_t) (juce::int64) findDeviceState (song, id)[schema::deviceSidechain]);
    const auto state = findTrackState (song, source);
    return state.hasType (schema::TRACK) && ! isFolderState (state) ? source : TrackID();
}

SidechainChoice describeSidechainSource (const Song& song, DeviceID id)
{
    const auto source = getSidechainSource (song, id);
    return source.isValid() ? describe (song, source) : SidechainChoice();
}

std::vector<SidechainChoice> describeTracksToListenTo (const Song& song, TrackID listener)
{
    std::vector<SidechainChoice> choices;
    const auto fed = findTrackState (song, listener).hasType (schema::TRACK) ? getTracksFedBy (song, listener) : std::vector<TrackID>();

    for (const auto track : getSoundTracks (song))
    {
        if (track == listener)
            continue;

        auto choice = describe (song, track);

        if (contains (fed, track))
            choice.whyNot = "its sound comes from " + getTrackName (song, listener);

        choices.push_back (choice);
    }

    return choices;
}

std::vector<SidechainChoice> describeSidechainChoices (const Song& song, DeviceID id)
{
    if (! hasSidechain (song, id))
        return {};

    return describeTracksToListenTo (song, getDeviceChannel (song, id));
}

void setSidechainSource (Song& song, DeviceID id, TrackID source)
{
    if (source == getSidechainSource (song, id) || (source.isValid() && ! contains (getSidechainSources (song, id), source)))
        return;

    auto state = findDeviceState (song, id);

    if (! state.isValid())
        return;

    const UndoStep step (song, source.isValid() ? "Sidechain from " + getTrackName (song, source) : juce::String ("Turn Sidechain Off"));
    auto* undo = &song.getUndoManager();

    if (source.isValid())
        state.setProperty (schema::deviceSidechain, (juce::int64) source.getRaw(), undo);
    else
        state.removeProperty (schema::deviceSidechain, undo);

    if (source.isValid())
        startDucking (song, id, source);
}

DeviceID duckUnder (Song& song, TrackID ducked, TrackID key)
{
    if (ducked == key || ! findTrackState (song, ducked).hasType (schema::TRACK) || contains (getTracksFedBy (song, ducked), key))
        return {};

    const UndoStep step (song, "Duck " + getTrackName (song, ducked) + " Under " + getTrackName (song, key));
    const auto compressor = insertDeviceState (song, ducked, makeOwnDeviceState (song, devices::Compressor::typeID));

    if (compressor.isValid())
        setSidechainSource (song, compressor, key);

    return compressor;
}

void forgetSidechainSources (Song& song, const std::vector<TrackID>& tracks)
{
    std::set<juce::int64> going;

    for (const auto track : tracks)
    {
        going.insert ((juce::int64) track.getRaw());

        for (const auto inside : getTracksInFolder (song, track))
            going.insert ((juce::int64) inside.getRaw());
    }

    if (going.empty())
        return;

    // The song's devices that listen to them, in containers too, listen to none.
    auto chains = getTrackStates (song);
    chains.push_back (song.getState().getChildWithName (schema::MASTER));

    const auto forget = [&song, &going] (juce::ValueTree device)
    {
        if (going.contains ((juce::int64) device[schema::deviceSidechain]))
            device.removeProperty (schema::deviceSidechain, &song.getUndoManager());
    };

    for (const auto& chain : chains)
    {
        for (const auto& device : getDeviceStates (chain))
        {
            forget (device);

            for (const auto& inside : getDeviceStates (device))
                forget (inside);
        }
    }
}
} // namespace phazite
