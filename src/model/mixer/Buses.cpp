// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Buses.h"
#include "internal/Mixer.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"

#include <utility>

namespace phazite
{
namespace
{
    ParameterRef getVolume (const Song& song, TrackID folder)
    {
        return { getFader (song, folder), getVolumeParameterID (song, folder) };
    }

    // A folder's volume as it was, its automation and modulation, and its lanes (view state, without an undo step), named
    // as its fader of the other kind names it, in the caller's step. What named its pan, which a plain folder's VCA hasn't,
    // its curve too, goes with the fader it had (forgetWhatNamesTheGone()).
    void followVolume (Song& song, TrackID folder, const ParameterRef& was)
    {
        const auto now = getVolume (song, folder);

        // Its curve, in its place among the song's (Automation.h).
        renameCurve (song, was, now);

        // Its modulations, each in its place (Modulation.h).
        renameModulated (song, was, now);

        // Its MIDI link, which turns it by its new name.
        if (const auto controller = findMidiLink (song, was); controller.has_value() && was != now)
            linkMidiController (song, *controller, now);

        // A lane for the new name after the old one's, as it was (its height, collapsed or not): the old one stays, ignored
        // while its fader is gone, for an undo that brings that fader back (lanes are view state, outside the history: the
        // folder's view, set once, Tracks.h: setTrackView()).
        const auto held = getTrackView (song, folder);
        auto view = held;
        view.lanes = {};

        for (const auto& lane : held.lanes)
        {
            view.lanes = view.lanes.push_back (lane);

            if (lane.device == was.device && lane.parameter == was.parameter)
            {
                auto copy = lane;
                copy.device = now.device;
                copy.parameter = now.parameter;
                view.lanes = view.lanes.push_back (std::move (copy));
            }
        }

        if (view.lanes.size() != held.lanes.size())
            setTrackView (song, folder, std::move (view));

        // A pass that writes the volume as the song plays goes on by its new name.
        if (auto* session = findSession (song))
            session->getWriter().parameterRenamed (was, now);

        forgetWhatNamesTheGone (song);
    }
} // namespace

bool isBus (const Song& song, TrackID track)
{
    // A folder whose volume a fader of its own sets, not a VCA (Schema.h: channelFader).
    return readIsBus (findTrackState (song, track));
}

void makeBus (Song& song, TrackID folder)
{
    if (! isFolder (song, folder) || isBus (song, folder))
        return;

    // A fader of its own in place of its VCA, which plays at the folder's volume (the song's: Schema.h channelVolume).
    const auto was = getVolume (song, folder);
    const auto fader = DeviceID::fromRaw (song.makeID());

    setTrackFields (song, folder,
                    [fader] (TrackValue& value)
                    {
                        value.vca = {};
                        value.fader = fader;
                    });

    followVolume (song, folder, was);
}

void makePlainFolder (Song& song, TrackID folder)
{
    if (! isBus (song, folder))
        return;

    // A VCA of its own in place of its fader, at the folder's volume; its devices go from the song too (what its plug-ins keep
    // of their own stored first, so that an undo brings them back as they were), and the sidechains that listened to it hear
    // the sound passing through them again.
    song.storeDeviceStates (folder.getRaw());

    for (const auto device : getDevices (song, folder))
        eraseDevice (song, device, getDeviceChannelPlace (song, folder));

    forgetSidechainsOfBus (song, folder);

    const auto was = getVolume (song, folder);
    const auto vca = DeviceID::fromRaw (song.makeID());

    setTrackFields (song, folder,
                    [vca] (TrackValue& value)
                    {
                        value.fader = {};
                        value.vca = vca;
                    });

    followVolume (song, folder, was); // and what named its devices goes with them
}

void mixAsBus (Song& song, TrackID folder)
{
    makeBus (song, folder);

    if (isFolder (song, folder))
        setTrackFields (song, folder, [] (TrackValue& value) { value.bus = true; });
}
} // namespace phazite
