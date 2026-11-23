// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sidechain.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Multiband.h"
#include "devices/vocoder/Vocoder.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>

namespace phazite
{
namespace
{
    bool contains (const std::vector<TrackID>& tracks, TrackID track)
    {
        return std::find (tracks.begin(), tracks.end(), track) != tracks.end();
    }

    // Whether a track has sound of its own for a sidechain to listen to: a track, a return, or a bus, which mixes the tracks
    // in it (as Ableton Live's, Logic's and Cubase's sidechains take group and bus channels); not a plain folder, which only
    // scales its tracks' volumes (Buses.h).
    bool hasSoundOfItsOwn (const juce::ValueTree& state)
    {
        return ! isFolderState (state) || state.hasProperty (schema::channelFader);
    }

    // The tracks with sound of their own, in the song's order.
    std::vector<TrackID> getSoundTracks (const Song& song)
    {
        std::vector<TrackID> tracks;

        for (const auto& state : getTrackStates (song))
            if (hasSoundOfItsOwn (state))
                tracks.push_back (getTrackID (state));

        return tracks;
    }

    // Whether a track's sound comes from any of these tracks: a folder mixes the tracks inside it, a return takes in what
    // is sent to it, a drum pad's track plays what its sampler's track's sampler plays for it (DrumKit.h), and a device can
    // listen to a sidechain.
    bool isFedBy (const Song& song, const juce::ValueTree& state, const std::vector<TrackID>& feeding)
    {
        const auto track = getTrackID (state);

        if (const auto pad = getPlayedPad (song, track); pad.has_value() && contains (feeding, getDeviceChannel (song, pad->first)))
            return true;

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
            if (contains (feeding, readDeviceSidechain (device)))
                return true;

        return false;
    }

    SidechainChoice describe (const Song& song, TrackID track)
    {
        const auto tracks = getTrackIDs (song);
        const auto row = (int) std::distance (tracks.begin(), std::find (tracks.begin(), tracks.end(), track));
        return { track, getTrackName (song, track), getTrackColour (song, track, row) };
    }

    // Whether a track plays a drum sampler, or, for a bus, a track in it does.
    bool playsDrumSampler (const Song& song, TrackID track)
    {
        const auto plays = [&song] (TrackID each)
        {
            const auto devices = getDeviceStates (findTrackState (song, each));
            return std::any_of (devices.begin(), devices.end(), [] (const juce::ValueTree& device) { return isDrumSampler (device); });
        };

        const auto inside = getTracksInFolder (song, track);
        return plays (track) || std::any_of (inside.begin(), inside.end(), plays);
    }

    // Points the sidechains that listen to any of these tracks back at the sound passing through each device, in containers
    // too, each through the devices' setter. Joins the caller's undo step.
    void forgetSidechainsListeningTo (Song& song, const std::set<juce::int64>& going)
    {
        if (going.empty())
            return;

        for (const auto& device : getSongDeviceOutline (song))
            if (device.sidechain.isValid() && going.contains ((juce::int64) device.sidechain.getRaw()))
                setDeviceFields (song, device.id, [] (DeviceValue& value) { value.sidechain = {}; });
    }

    // Whether every parameter of a device's is still as it was made, as the song holds its values set by hand, but the one
    // given, if any.
    bool isAsMade (const juce::ValueTree& state, const devices::DeviceInfo& info, std::string_view except = {})
    {
        for (const auto& parameter : info.parameters)
            if (parameter.id != except && std::abs (readDeviceParameterValue (state, parameter) - parameter.defaultValue) > 1.0e-4f)
                return false;

        return true;
    }

    bool isCompressor (const juce::ValueTree& state)
    {
        return readDeviceType (state) == juce::String (devices::Compressor::typeID);
    }

    // Phazite's compressor, still as it was made (but for its mode, set by its Duck tab first), ducks under the key chosen for
    // it (its Duck mode), joining the caller's step (setSidechainSource()); one set its own way keeps its mode, and compressing,
    // it compresses as its key says (a keyed compressor). As made, it is set up to pump under a kick (docs/design.md,
    // section 8): from -14 dB, 14 dB down, in 1 ms, held 120 ms
    // from the hit and as long as the key stays over -14 dB, and back in a straight line over 80 ms. When the key plays a drum
    // sampler it hears only its kick's body from 60 to 150 Hz: below 60 Hz a kick's long boom would hold it down through the
    // beat, and the snare, clap and hats on the same track sound above 150 Hz. Under every factory kit's kick at velocity 100 at
    // 120 BPM it ducks 6 to 14 dB and is back within 3 dB in 180 to 270 ms, and no kit's snare, clap, hats or cymbals move it
    // (measured as docs/presets.md measures the sidechain presets). And a multiband still as it was made, its low band, which
    // then hears the key, so that the lows duck under it and the rest plays on (every band's Key is off at first, and a band at
    // first limits at 0 dB, which a kick under full scale never reaches).
    void startDucking (Song& song, DeviceID id, TrackID source)
    {
        using devices::Compressor, devices::Multiband;
        const auto state = findDeviceState (song, id);
        const auto setValue = [&song, &state] (const devices::DeviceInfo& info, std::string_view parameter, float value)
        {
            if (const auto index = devices::findParameter (info, parameter); index >= 0)
                setDeviceValue (song, state, info.parameters[(size_t) index], value);
        };

        if (isCompressor (state))
        {
            const auto& info = Compressor::info;
            if (! isAsMade (state, info, info.parameters[(size_t) Compressor::mode].id))
                return;

            setValue (info, "mode", (float) Compressor::Mode::duck);
            setValue (info, "threshold", -14.0f);
            setValue (info, "depth", 14.0f);
            setValue (info, "attack", 1.0f);
            setValue (info, "hold", 120.0f);
            setValue (info, "release", 80.0f);
            setValue (info, "releaseCurve", 0.0f);

            if (playsDrumSampler (song, source))
            {
                setValue (info, "keyLowCut", 60.0f);
                setValue (info, "keyHighCut", 150.0f);
            }

            return;
        }

        if (readDeviceType (state) != juce::String (Multiband::typeID) || ! isAsMade (state, Multiband::info))
            return;

        // Its low band's curve: 8:1 over -30 dB, its last point in the places the others leave.
        namespace curves = devices::curves;
        const auto& info = Multiband::info;
        const auto firstPoint = Multiband::getBandParameter (Multiband::low, Multiband::points);
        const auto points = curves::makeCompression (-30.0f, 8.0f);
        setValue (info, info.parameters[(size_t) firstPoint].id, (float) points.size());

        for (int point = 0; point < curves::maxPoints; ++point)
            for (int field = 0; field < curves::pointFields; ++field)
                setValue (info, info.parameters[(size_t) (firstPoint + curves::getPointParameter (point, (curves::PointField) field))].id,
                          points[(size_t) std::min (point, (int) points.size() - 1)][(size_t) field]);

        // Quick to grip, and letting go over the second release, the slowing one, as long.
        setValue (info, "lowKey", 1.0f);
        setValue (info, "lowAttack", 1.0f);
        setValue (info, "lowRelease2", 150.0f);

        if (playsDrumSampler (song, source))
            setValue (info, "keyHighCut", 150.0f);
    }

    // A vocoder plays the track it listens to: its carrier becomes Track, joining the caller's step (setSidechainSource()),
    // so that the track chosen is heard at once, whatever carrier it had.
    void startCarrying (Song& song, DeviceID id)
    {
        using devices::Vocoder;
        const auto state = findDeviceState (song, id);

        if (readDeviceType (state) == juce::String (Vocoder::typeID))
            setDeviceValue (song, state, Vocoder::info.parameters[(size_t) Vocoder::carrier], (float) Vocoder::Carrier::track);
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

    if (const auto* own = findOwnDevice (readDeviceType (state).toStdString()))
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
    const auto source = readDeviceSidechain (findDeviceState (song, id));
    const auto state = findTrackState (song, source);
    return state.hasType (schema::TRACK) && hasSoundOfItsOwn (state) ? source : TrackID();
}

bool hearsSidechain (const Song& song, DeviceID id)
{
    if (! getSidechainSource (song, id).isValid())
        return false;

    const auto state = findDeviceState (song, id);
    const auto& mode = devices::Compressor::info.parameters[(size_t) devices::Compressor::mode];
    return ! isCompressor (state)
           || devices::Compressor::getMode (readDeviceParameterValue (state, mode)) != devices::Compressor::Mode::transient;
}

bool switchesSidechain (std::string_view typeID, int parameterIndex)
{
    return typeID == std::string_view (devices::Compressor::typeID) && parameterIndex == (int) devices::Compressor::mode;
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

    for (const auto& state : getTrackStates (song))
    {
        const auto track = getTrackID (state);

        if (track == listener)
            continue;

        auto choice = describe (song, track);

        if (! hasSoundOfItsOwn (state))
            choice.whyNot = "a plain folder: mix it as a bus to listen to it";
        else if (contains (fed, track))
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
    setDeviceFields (song, state, [source] (DeviceValue& value) { value.sidechain = source; });

    if (source.isValid())
    {
        startDucking (song, id, source);
        startCarrying (song, id);
    }
}

DeviceID duckUnder (Song& song, TrackID ducked, TrackID key)
{
    const auto keyState = findTrackState (song, key);

    if (ducked == key || ! findTrackState (song, ducked).hasType (schema::TRACK) || ! keyState.hasType (schema::TRACK)
        || ! hasSoundOfItsOwn (keyState) || contains (getTracksFedBy (song, ducked), key))
        return {};

    const UndoStep step (song, "Duck " + getTrackName (song, ducked) + " Under " + getTrackName (song, key));
    const auto compressor = insertDevice (song, ducked, makeOwnDevice (song, devices::Compressor::typeID));

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

    forgetSidechainsListeningTo (song, going);
}

void forgetSidechainsOfBus (Song& song, TrackID bus)
{
    forgetSidechainsListeningTo (song, { (juce::int64) bus.getRaw() });
}

void forgetSidechainLoops (Song& song)
{
    // One at a time, so that of two devices listening to each other's tracks only the first lets go: the other's is no loop
    // then. A device in a container has no sidechain (Containers.h), nor does the master's listen to a loop.
    for (const auto& device : getSongDeviceOutline (song))
    {
        if (! device.sidechain.isValid() || device.container.isValid() || ! findTrackState (song, device.channel).hasType (schema::TRACK))
            continue;

        if (contains (getTracksFedBy (song, device.channel), device.sidechain))
            setDeviceFields (song, device.id, [] (DeviceValue& value) { value.sidechain = {}; });
    }
}
} // namespace phazite
