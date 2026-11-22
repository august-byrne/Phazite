// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Buses.h"
#include "internal/Mixer.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"

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
    // goes with the fader it had (forgetWhatNamesTheGone()); a curve stays while an undo could bring that fader back.
    void followVolume (Song& song, TrackID folder, const ParameterRef& was)
    {
        auto* undo = &song.getUndoManager();
        const auto now = getVolume (song, folder);
        const auto device = juce::String ((juce::int64) now.device.getRaw());

        // Its curve, in its place among the song's (Automation.h).
        renameCurve (song, was, now);

        for (const auto& modulator : getModulatorStates (song))
        {
            for (auto modulates : modulator)
            {
                if (modulates.hasType (schema::MODULATES) && getModulatedParameter (modulates) == was)
                {
                    modulates.setProperty (schema::modulatesDevice, device, undo);
                    modulates.setProperty (schema::modulatesParameter, now.parameter, undo);
                }
            }
        }

        // Its MIDI link, which turns it by its new name.
        if (const auto controller = findMidiLink (song, was); controller.has_value() && was != now)
            linkMidiController (song, *controller, now);

        // A lane for the new name after the old one's, as it was (its height, collapsed or not): the old one stays, ignored
        // while its fader is gone, for an undo that brings that fader back (lanes are view state, outside the history).
        auto lanes = findTrackState (song, folder).getChildWithName (schema::LANES);

        for (auto i = lanes.getNumChildren(); --i >= 0;)
        {
            if (const auto lane = lanes.getChild (i);
                lane[schema::laneDevice].toString() == juce::String ((juce::int64) was.device.getRaw())
                && lane[schema::laneParameter].toString() == was.parameter)
            {
                auto copy = lane.createCopy();
                copy.setProperty (schema::laneDevice, device, nullptr);
                copy.setProperty (schema::laneParameter, now.parameter, nullptr);
                lanes.addChild (copy, i + 1, nullptr);
            }
        }

        // A pass that writes the volume as the song plays goes on by its new name.
        if (auto* session = findSession (song))
            session->getWriter().parameterRenamed (was, now);

        forgetWhatNamesTheGone (song);
    }
} // namespace

bool isBus (const Song& song, TrackID track)
{
    // A folder whose volume a fader of its own sets, not a VCA (Schema.h: channelFader).
    const auto state = findTrackState (song, track);
    return isFolderState (state) && state.hasProperty (schema::channelFader);
}

void makeBus (Song& song, TrackID folder)
{
    if (! isFolder (song, folder) || isBus (song, folder))
        return;

    // A fader of its own in place of its VCA, which plays at the folder's volume (the song's: Schema.h channelVolume).
    auto* undo = &song.getUndoManager();
    auto state = findTrackState (song, folder);
    const auto was = getVolume (song, folder);
    state.removeProperty (schema::channelVCA, undo);
    state.setProperty (schema::channelFader, (juce::int64) song.makeID(), undo);
    followVolume (song, folder, was);
}

void makePlainFolder (Song& song, TrackID folder)
{
    if (! isBus (song, folder))
        return;

    // A VCA of its own in place of its fader, at the folder's volume; its devices go from the song too, and the sidechains
    // that listened to it hear the sound passing through them again.
    auto* undo = &song.getUndoManager();
    auto state = findTrackState (song, folder);

    for (const auto& device : getDeviceStates (state))
        state.removeChild (device, undo);

    forgetSidechainsOfBus (song, folder);

    const auto was = getVolume (song, folder);
    state.removeProperty (schema::channelFader, undo);
    state.setProperty (schema::channelVCA, (juce::int64) song.makeID(), undo);
    followVolume (song, folder, was); // and what named its devices goes with them
}

void mixAsBus (Song& song, TrackID folder)
{
    makeBus (song, folder);

    if (auto state = findTrackState (song, folder); isFolderState (state))
        state.setProperty (schema::folderBus, true, &song.getUndoManager());
}
} // namespace phazite
