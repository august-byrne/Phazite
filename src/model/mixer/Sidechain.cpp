// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sidechain.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Multiband.h"
#include "devices/vocoder/Vocoder.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/TracksValue.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

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
    // Read from a track's value (TracksValue.h), or by its ID.
    bool hasSoundOfItsOwn (const TrackValue& value)
    {
        return ! value.isFolder() || value.isBus();
    }

    // Whether the song holds a track (a track, a folder or a return, not the master) by its ID.
    bool isSongTrack (const Song& song, TrackID track)
    {
        const auto kind = getTrackKind (song, track);
        return kind.has_value() && *kind != TrackValue::Kind::master;
    }

    bool hasSoundOfItsOwn (const Song& song, TrackID track)
    {
        return isSongTrack (song, track) && (! isFolder (song, track) || isBus (song, track));
    }

    // The tracks with sound of their own, in the song's order.
    std::vector<TrackID> getSoundTracks (const Song& song)
    {
        std::vector<TrackID> tracks;
        const auto& values = getTracksValue (song);

        for (const auto id : getTrackOrder (song).order)
            if (const auto* value = values.tracks.find (id.getRaw()); value != nullptr && hasSoundOfItsOwn (*value))
                tracks.push_back (id);

        return tracks;
    }

    // The channel a device or a container is on, by its value: a track's ID, or the master's (as Mixer.h's getDeviceChannel()
    // finds it); none for a chain or an ID the devices haven't.
    TrackID findChannelIn (const SongTracks& tracks, const SongDevices& devices, DeviceID device)
    {
        const auto* value = devices.find (device);

        if (value == nullptr || value->form == DeviceValue::Form::chain)
            return {};

        return value->place.onMaster ? tracks.master.id : value->place.track;
    }

    // Whether a track's sound comes from any of these tracks: a folder mixes the tracks inside it, a return takes in what
    // is sent to it, a drum pad's track plays what its sampler's track's sampler plays for it (DrumKit.h), and a device can
    // listen to a sidechain. Of the song's values given (a song's areas, or a file's tree's read once by the loader:
    // forgetSidechainLoopsIn()).
    bool isFedBy (const SongTracks& tracks, const SongDevices& devices, const SongSends& sends, const TrackValue& value,
                  const std::vector<TrackID>& feeding)
    {
        const auto track = value.id;
        const auto kind = value.kind;

        if (value.pad.has_value() && contains (feeding, findChannelIn (tracks, devices, value.padDevice)))
            return true;

        // Every track inside a folder, in folders inside it too.
        const std::function<bool (TrackID)> holdsFeeding = [&tracks, &feeding, &holdsFeeding] (TrackID folder)
        {
            if (const auto* list = tracks.findList (folder))
                for (const auto inside : *list)
                    if (contains (feeding, inside) || holdsFeeding (inside))
                        return true;

            return false;
        };

        if (kind == TrackValue::Kind::folder && holdsFeeding (track))
            return true;

        if (kind == TrackValue::Kind::returnTrack)
            for (const auto sender : feeding)
                if (const auto* list = sends.tracks.find (sender.getRaw()))
                    for (const auto send : *list)
                        if (const auto* found = sends.find (send); found != nullptr && found->to == track)
                            return true;

        if (const auto* list = devices.findList (makeChannelPlace (track, false)))
            for (const auto id : *list)
                if (const auto* device = devices.find (id); device != nullptr && contains (feeding, device->sidechain))
                    return true;

        return false;
    }

    // The tracks whose sound comes from a track's, the track first (getTracksFedBy()), of the song's values given, the tracks
    // in the song's order.
    std::vector<TrackID> findTracksFedBy (const SongTracks& tracks, const std::vector<TrackID>& order, const SongDevices& devices,
                                          const SongSends& sends, TrackID track)
    {
        std::vector<TrackID> fed { track };

        for (auto grew = true; grew;)
        {
            grew = false;

            for (const auto id : order)
            {
                if (const auto* other = tracks.tracks.find (id.getRaw());
                    other != nullptr && ! contains (fed, id) && isFedBy (tracks, devices, sends, *other, fed))
                {
                    fed.push_back (id);
                    grew = true;
                }
            }
        }

        return fed;
    }

    // The devices whose sidechains are loops (forgetSidechainLoops()), of the song's values given, one at a time, so that of two
    // devices listening to each other's tracks only the first lets go: the other's is no loop then. Each device on a track's
    // channel in the song's order (a device in a container has no sidechain: Containers.h, nor does the master's listen to a
    // loop), its sidechain taken away in the devices given as it is found.
    std::vector<DeviceID> findSidechainLoops (const SongTracks& tracks, const std::vector<TrackID>& order, SongDevices devices,
                                              const SongSends& sends)
    {
        std::vector<DeviceID> loops;

        for (const auto track : order)
        {
            const auto* list = devices.findList (makeChannelPlace (track, false));

            if (list == nullptr)
                continue;

            for (const auto id : std::vector<DeviceID> (list->begin(), list->end()))
            {
                const auto* device = devices.find (id);

                if (device == nullptr || ! device->sidechain.isValid() || device->place.container.isValid())
                    continue;

                if (! contains (findTracksFedBy (tracks, order, devices, sends, track), device->sidechain))
                    continue;

                auto cut = *device;
                cut.sidechain = {};
                devices.devices = devices.devices.set (id.getRaw(), std::move (cut));
                loops.push_back (id);
            }
        }

        return loops;
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
            const auto devices = getDevices (song, each); // the devices' (Mixer.h)
            return std::any_of (devices.begin(), devices.end(), [&song] (DeviceID device) { return isDrumSampler (song, device); });
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
    bool isAsMade (const DeviceValue& value, const devices::DeviceInfo& info, std::string_view except = {})
    {
        for (size_t i = 0; i < info.parameters.size(); ++i)
            if (const auto& parameter = info.parameters[i];
                parameter.id != except && std::abs (value.getParameterValue ((int) i) - parameter.defaultValue) > 1.0e-4f)
                return false;

        return true;
    }

    // A device's value of one of Phazite's own types, by its type's ID: nullptr for none, or for another type.
    const DeviceValue* findOfType (const Song& song, DeviceID id, std::string_view typeID)
    {
        const auto* value = findDevice (song, id);
        return value != nullptr && value->form == DeviceValue::Form::own && std::string_view (value->type.toRawUTF8()) == typeID ? value
                                                                                                                                 : nullptr;
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
        const auto setValue = [&song, id] (const devices::DeviceInfo& info, std::string_view parameter, float value)
        {
            if (const auto index = devices::findParameter (info, parameter); index >= 0)
                setDeviceValue (song, id, index, value);
        };

        if (const auto* compressor = findOfType (song, id, Compressor::typeID))
        {
            const auto& info = Compressor::info;
            if (! isAsMade (*compressor, info, info.parameters[(size_t) Compressor::mode].id))
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

        if (const auto* multiband = findOfType (song, id, Multiband::typeID);
            multiband == nullptr || ! isAsMade (*multiband, Multiband::info))
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
        if (findOfType (song, id, Vocoder::typeID) != nullptr)
            setDeviceValue (song, id, (int) Vocoder::carrier, (float) Vocoder::Carrier::track);
    }
} // namespace

std::vector<TrackID> getTracksFedBy (const Song& song, TrackID track)
{
    return findTracksFedBy (getTracksValue (song), getTrackOrder (song).order, getDevicesValue (song), getSendsValue (song), track);
}

bool hasSidechain (const Song& song, DeviceID id)
{
    const auto* value = findDevice (song, id);

    // A container wires only the sound passing through its devices (Containers.h).
    if (value == nullptr || ! (value->form == DeviceValue::Form::own || value->isPlugin()) || value->place.container.isValid())
        return false;

    if (value->form == DeviceValue::Form::own)
    {
        const auto* own = findOwnDevice (std::string_view (value->type.toRawUTF8()));
        return own != nullptr && own->hasSidechain;
    }

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
    const auto fed = isSongTrack (song, channel) ? getTracksFedBy (song, channel) : std::vector<TrackID>();

    for (const auto track : getSoundTracks (song))
        if (! contains (fed, track))
            sources.push_back (track);

    return sources;
}

TrackID getSidechainSource (const Song& song, DeviceID id)
{
    const auto* value = findDevice (song, id);
    const auto source = value != nullptr ? value->sidechain : TrackID();
    return hasSoundOfItsOwn (song, source) ? source : TrackID();
}

bool hearsSidechain (const Song& song, DeviceID id)
{
    if (! getSidechainSource (song, id).isValid())
        return false;

    const auto* compressor = findOfType (song, id, devices::Compressor::typeID);
    return compressor == nullptr
           || devices::Compressor::getMode (compressor->getParameterValue ((int) devices::Compressor::mode))
                  != devices::Compressor::Mode::transient;
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
    const auto fed = isSongTrack (song, listener) ? getTracksFedBy (song, listener) : std::vector<TrackID>();

    const auto tracks = getTracksValue (song); // a copy, which shares what it holds

    for (const auto track : getTrackIDs (song))
    {
        const auto* value = tracks.tracks.find (track.getRaw());

        if (track == listener || value == nullptr)
            continue;

        auto choice = describe (song, track);

        if (! hasSoundOfItsOwn (*value))
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

    if (findDevice (song, id) == nullptr)
        return;

    const UndoStep step (song, source.isValid() ? "Sidechain from " + getTrackName (song, source) : juce::String ("Turn Sidechain Off"));
    setDeviceFields (song, id, [source] (DeviceValue& value) { value.sidechain = source; });

    if (source.isValid())
    {
        startDucking (song, id, source);
        startCarrying (song, id);
    }
}

DeviceID duckUnder (Song& song, TrackID ducked, TrackID key)
{
    if (ducked == key || ! isSongTrack (song, ducked) || ! hasSoundOfItsOwn (song, key) || contains (getTracksFedBy (song, ducked), key))
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
    for (const auto id :
         findSidechainLoops (getTracksValue (song), getTrackOrder (song).order, getDevicesValue (song), getSendsValue (song)))
        setDeviceFields (song, id, [] (DeviceValue& value) { value.sidechain = {}; });
}

void forgetSidechainLoopsIn (juce::ValueTree& song)
{
    // By the one rule, of the tree's devices (their own fields alone) read once, and, where one listens to a sidechain, its
    // tracks and sends read once; each found device's element, on a track's channel, without its sidechain.
    auto devices = readDevicesFields (song);
    auto listens = false;

    for (const auto& entry : devices.devices)
        listens = listens || entry.second.sidechain.isValid();

    if (! listens)
        return;

    const auto loops = findSidechainLoops (readTracksValue (song), readTrackOrder (song).order, std::move (devices), readSendsValue (song));

    if (loops.empty())
        return;

    for (auto track : listTrackElements (song))
        for (auto device : track)
            if (device.hasType (schema::DEVICE) && std::find (loops.begin(), loops.end(), readDeviceElementID (device)) != loops.end())
                device.removeProperty (schema::deviceSidechain, nullptr);
}
} // namespace phazite
