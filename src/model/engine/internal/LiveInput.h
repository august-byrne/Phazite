// SPDX-License-Identifier: AGPL-3.0-or-later
// Live MIDI input: what is played on a MIDI keyboard or the computer keyboard (AudioIO::playLive(), AudioIO.h) goes to one
// track of the open project, which Phazite's engine plays (model/playback/internal/PlaybackSession.h keeps which track, by
// its ID). Whether a track has an instrument that it can play is hasInstrument() (model/mixer/internal/Mixer.h).
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

namespace phazite
{
// The track that live MIDI input plays, or none: none yet, one that went, or a song without a session (none that a
// ProjectContext holds).
TrackID getLiveMidiTrack (const Song&);

// Makes live MIDI input play this track, and no other. Returns false if the song has no session, or the track isn't one of
// its tracks (a folder, the master or one that is gone).
bool setLiveMidiTrack (Song&, TrackID);

// If live MIDI input plays none of the song's tracks (none yet, or one that went), sends it to the first track with an
// instrument, or nowhere. Call it when a project opens and whenever its tracks change.
void routeLiveMidiInput (Song&);

// The track the selection is about: the first selected track (not a folder), or else the first selected clip's, the track
// of the clip whose notes are selected, or the track of the selected devices; none for none of these.
TrackID getSelectedTrack (const Selection&);

// Live MIDI input follows the selection, as FL Studio's selected channel does: if the selected track (see
// getSelectedTrack()) has an instrument, it plays that track. Call it when the selection changes.
void followSelectedTrack (const Selection&);
} // namespace phazite
