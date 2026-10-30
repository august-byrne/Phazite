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

    // The track before a track in its folder, or at the top level: Tracktion's place for it.
    te::Track* findTrackBefore (te::Track& track)
    {
        const auto parent = track.state.getParent();

        for (auto i = parent.indexOf (track.state); --i >= 0;)
            if (auto* previous = te::findTrackForState (track.edit, parent.getChild (i)))
                return previous;

        return nullptr;
    }

    // The switches Tracktion plays a track with, which it keeps outside its undo history: mute, solo, and solo isolation
    // (a return's).
    void setSwitches (te::Track& track, const juce::ValueTree& state)
    {
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
                juce::ValueTree created (isFolder ? te::IDs::FOLDERTRACK : te::IDs::TRACK);
                id.writeID (created, nullptr);
                track = edit.insertTrack (te::TrackInsertPoint (folder, preceding), created, nullptr).get();

                if (track == nullptr)
                    continue;

                // As Tracktion makes a track or a plain folder (Phazite makes a folder a bus: model/mixer/internal/Buses.h).
                track->pluginList.addDefaultTrackPlugins (isFolder);
            }
            else if (track->getParentTrack() != folder || findTrackBefore (*track) != preceding)
            {
                edit.moveTrack (track, te::TrackInsertPoint (folder, preceding));
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
}

void reconcileTrackSwitches (te::Edit& edit)
{
    for (const auto& state : getTrackStates (edit))
        if (auto* track = te::findTrackForID (edit, toTracktion (getTrackID (state))))
            setSwitches (*track, state);
}
} // namespace phazite::mirror
