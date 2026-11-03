// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"

#include <set>

namespace phazite::mirror
{
namespace
{
    bool isPlaylistTrack (const te::Track& track)
    {
        return track.isAudioTrack() || track.isFolderTrack();
    }

    // The last of Tracktion's global tracks (tempo, markers, chords, the arranger), which the song's tracks follow.
    te::Track* findLastGlobalTrack (te::Edit& edit)
    {
        te::Track* last = nullptr;

        edit.getTrackList().visitAllTopLevel (
            [&last] (te::Track& track)
            {
                if (isPlaylistTrack (track))
                    return false;

                last = &track;
                return true;
            });

        return last;
    }

    // The states Tracktion makes a channel's own devices from, as the song has them (Schema.h: channelFader, channelVCA):
    // a fader, with its meter after it, or a plain folder's VCA, with the song's ID. None for a track the song gives none.
    juce::Array<juce::ValueTree> makeChannelDevices (const juce::ValueTree& state)
    {
        const auto isVCA = state.hasProperty (schema::channelVCA);
        const auto id = te::EditItemID::fromVar (state[isVCA ? schema::channelVCA : schema::channelFader]);

        if (! id.isValid())
            return {};

        auto fader = isVCA ? te::VCAPlugin::create() : te::VolumeAndPanPlugin::create();
        id.writeID (fader, nullptr);

        if (isVCA)
            return juce::Array<juce::ValueTree> (fader);

        return juce::Array<juce::ValueTree> (fader, te::LevelMeterPlugin::create());
    }

    bool isChannelFader (const te::Plugin& plugin)
    {
        return dynamic_cast<const te::VolumeAndPanPlugin*> (&plugin) != nullptr || dynamic_cast<const te::VCAPlugin*> (&plugin) != nullptr;
    }

    // A channel's own device into its list, before the one at an index (or last), through the list's state: a folder that
    // becomes a bus takes its fader so without Tracktion warning that it does.
    void insertChannelDevice (te::Edit& edit, te::PluginList& list, const juce::ValueTree& state, int index)
    {
        const auto device = edit.getPluginCache().createNewPlugin (state);
        const auto at = juce::isPositiveAndBelow (index, list.size()) ? list.state.indexOf (list[index]->state) : -1;
        list.state.addChild (device->state, at, &edit.getUndoManager());
    }

    // A channel's own devices in Tracktion as the song has them: its fader, or a plain folder's VCA, with the song's ID. One
    // of the other kind or with another ID goes (and the meter after a fader, for a VCA), and the song's takes its place,
    // with a meter after a fader that has none.
    void reconcileChannelDevices (te::Edit& edit, te::Track& track, const juce::ValueTree& state)
    {
        const auto wanted = makeChannelDevices (state);

        if (wanted.isEmpty())
            return;

        auto& list = track.pluginList;
        const auto wantedID = te::EditItemID::fromID (wanted.getFirst());
        const auto wantedType = wanted.getFirst()[te::IDs::type].toString();

        for (auto* plugin : list)
            if (isChannelFader (*plugin) && plugin->itemID == wantedID && plugin->state[te::IDs::type].toString() == wantedType)
                return;

        auto index = -1;

        for (int i = list.size(); --i >= 0;)
        {
            if (auto* plugin = list[i]; isChannelFader (*plugin) || (wanted.size() == 1 && dynamic_cast<te::LevelMeterPlugin*> (plugin)))
            {
                index = isChannelFader (*plugin) ? i : index;
                plugin->deleteFromParent();
            }
        }

        // Without one to take the place of, a fader goes before the meter.
        if (const auto* meter = list.findFirstPluginOfType<te::LevelMeterPlugin>(); index < 0 && meter != nullptr)
            index = list.indexOf (meter);

        insertChannelDevice (edit, list, wanted.getFirst(), index);

        if (wanted.size() > 1 && list.getPluginsOfType<te::LevelMeterPlugin>().isEmpty())
            insertChannelDevice (edit, list, wanted[1], -1);
    }

    // The track before a track in its folder, or at the top level: Tracktion's place for it.
    te::Track* findTrackBefore (te::Track& track)
    {
        const auto parent = track.state.getParent();

        for (auto i = parent.indexOf (track.state); --i >= 0;)
            if (auto* previous = te::findTrackForState (track.edit, parent.getChild (i)))
                return previous;

        return nullptr;
    }

    // How Tracktion writes a track's automation as the song plays, which Phazite sets outside the undo history, as the song
    // keeps it (Schema.h: trackAutomationMode).
    void setAutomationMode (te::Track& track, const juce::ValueTree& state)
    {
        const auto mode = state[schema::trackAutomationMode].toString();
        auto wanted = te::AutomationMode::read;

        if (mode == schema::automationTouch)
            wanted = te::AutomationMode::touch;
        else if (mode == schema::automationLatch)
            wanted = te::AutomationMode::latch;
        else if (mode == schema::automationWrite)
            wanted = te::AutomationMode::write;

        if (track.automationMode.get() != wanted)
            track.state.setProperty (te::IDs::automationMode, te::toString (wanted), nullptr);
    }

    // The switches Tracktion plays a track with, which it keeps outside its undo history: mute, solo, and solo isolation
    // (a return's); and its write mode.
    void setSwitches (te::Track& track, const juce::ValueTree& state)
    {
        setAutomationMode (track, state);

        if (track.isMuted (false) != (bool) state[schema::trackMute])
            track.setMute (state[schema::trackMute]);

        if (track.isSolo (false) != (bool) state[schema::trackSolo])
            track.setSolo (state[schema::trackSolo]);

        if (track.isSoloIsolate (false) != isReturnState (state))
            track.setSoloIsolate (isReturnState (state));
    }

    // Tracktion's tracks for a parent's track elements, in their order, in a folder (or at the top level) after another
    // track: made if they aren't there, moved if they are elsewhere, with the mute, solo and solo isolation they play.
    void place (te::Edit& edit, const juce::ValueTree& parent, te::FolderTrack* folder, te::Track* preceding)
    {
        for (const auto& state : parent)
        {
            if (! state.hasType (schema::TRACK))
                continue;

            const auto id = toTracktion (getTrackID (state));
            const auto isFolder = isFolderState (state);
            auto* track = te::findTrackForID (edit, id);

            if (track == nullptr)
            {
                // With its own devices as the song has them, or as Tracktion makes a track's or a plain folder's.
                juce::ValueTree created (isFolder ? te::IDs::FOLDERTRACK : te::IDs::TRACK);
                id.writeID (created, nullptr);
                const auto channelDevices = makeChannelDevices (state);

                for (const auto& device : channelDevices)
                    created.appendChild (device, nullptr);

                track = edit.insertTrack (te::TrackInsertPoint (folder, preceding), created, nullptr).get();

                if (track == nullptr)
                    continue;

                if (channelDevices.isEmpty())
                    track->pluginList.addDefaultTrackPlugins (isFolder);
            }
            else
            {
                if (track->getParentTrack() != folder || findTrackBefore (*track) != preceding)
                    edit.moveTrack (track, te::TrackInsertPoint (folder, preceding));

                reconcileChannelDevices (edit, *track, state);
            }

            // Its name and color too, which Tracktion doesn't play with: so that what reads them from Tracktion while T1
            // lasts reads the song's.
            if (const auto name = state[schema::trackName].toString(); track->state[te::IDs::name].toString() != name)
                track->setName (name);

            if (const auto colour = juce::Colour::fromString (state[schema::trackColour].toString()); track->getColour() != colour)
                track->setColour (colour);

            setSwitches (*track, state);

            if (auto* asFolder = dynamic_cast<te::FolderTrack*> (track))
                place (edit, state, asFolder, nullptr);

            preceding = track;
        }
    }
} // namespace

void reconcileTracks (te::Edit& edit)
{
    const auto list = findTrackList (edit);

    if (! list.isValid())
        return;

    place (edit, list, nullptr, findLastGlobalTrack (edit));

    // Tracktion's tracks the song doesn't have go, with what they hold; a folder takes its tracks along.
    std::set<te::EditItemID> kept;

    for (const auto& state : getTrackStates (edit))
        kept.insert (toTracktion (getTrackID (state)));

    juce::Array<te::Track::Ptr> gone;

    for (auto* track : te::getAllTracks (edit))
        if (isPlaylistTrack (*track) && ! kept.contains (track->itemID))
            gone.add (track);

    for (auto& track : gone)
        if (te::findTrackForID (edit, track->itemID) == track.get())
            edit.deleteTrack (track.get());

    // The Edit's elements in the order Tracktion keeps them (the rest, then its global tracks, the master and the song's),
    // in the change's step: Tracktion would sort them a moment after a track is added, as an undo step of its own (an Edit
    // built from the song would have one as the project opens).
    te::TrackList::sortTracksByType (edit.state, &edit.getUndoManager());

    if (auto* master = edit.getMasterTrack())
        setAutomationMode (*master, findPhaziteState (edit).getChildWithName (schema::MASTER));
}

void reconcileTrackSwitches (te::Edit& edit)
{
    for (const auto& state : getTrackStates (edit))
        if (auto* track = findTrack (edit, state))
            setSwitches (*track, state);

    // The master's write mode, from its own element.
    if (auto* master = edit.getMasterTrack())
        setAutomationMode (*master, findPhaziteState (edit).getChildWithName (schema::MASTER));
}

void reconcileAll (te::Edit& edit)
{
    reconcileTracks (edit);
    reconcileTempoMap (edit);
    reconcileClips (edit);
    reconcileDevices (edit);
    reconcileAutomation (edit);
    reconcileTransport (edit);
}

void reconcile (te::Edit& edit, Song::Part part, std::uint64_t id)
{
    switch (part)
    {
        case Song::Part::clip:
            reconcileClip (edit, ClipID::fromRaw (id));
            break;
        case Song::Part::tempoMap:
            reconcileTempoMap (edit);
            break;
        case Song::Part::tracks:
            reconcileTracks (edit);
            break;
        case Song::Part::clips:
            reconcileClips (edit);
            break;
        case Song::Part::automation:
            reconcileAutomation (edit);
            break;
        case Song::Part::devices:
            reconcileDevices (edit);
            break;
        case Song::Part::modulation:
            reconcileModulation (edit);
            break;
        case Song::Part::transport:
            reconcileTransport (edit);
            break;
        case Song::Part::everything:
            reconcileAll (edit);
            break;
    }
}
} // namespace phazite::mirror
