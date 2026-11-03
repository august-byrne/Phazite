// SPDX-License-Identifier: AGPL-3.0-or-later
// Live MIDI input: what is played on a MIDI keyboard or the computer keyboard goes to one track of the open project, which
// Phazite's engine plays (AudioIO.h; model/playback/internal/PlaybackSession.h keeps which track, by its ID).
#pragma once

#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

namespace phazite
{
// Plays a note-on or note-off into live MIDI as a MIDI keyboard would (the computer keyboard's notes): into the track it
// plays, through Phazite's engine while the engine plays that track's project. Other messages do nothing.
void playLiveMidi (te::Engine&, const juce::MidiMessage&);

// The track that live MIDI input plays, or nullptr: none yet, or an Edit no ProjectContext holds (which has no session).
te::AudioTrack* getLiveMidiTrack (te::Edit&);

// Makes live MIDI input play this track, and no other. Returns false if its Edit has no session.
bool setLiveMidiTrack (te::AudioTrack&);

// If live MIDI input plays none of the Edit's tracks (none yet, or one that went), sends it to the first track with an
// instrument, or nowhere. Call it when a project opens and whenever its tracks change.
void routeLiveMidiInput (te::Edit&);

// True if the track has an instrument (a synth or sampler plug-in) that live MIDI input can play.
bool hasInstrument (const te::AudioTrack&);

// The track the selection is about: the first selected audio track, or else the first selected clip's, the track of the
// clip whose notes are selected, or the track of the selected devices.
te::AudioTrack* getSelectedAudioTrack (const Selection&);

// Live MIDI input follows the selection, as FL Studio's selected channel does: if the selected track (see
// getSelectedAudioTrack()) has an instrument, it plays that track. Call it when the selection changes.
void followSelectedTrack (const Selection&);
} // namespace phazite
