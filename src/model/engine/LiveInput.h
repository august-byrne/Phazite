// SPDX-License-Identifier: AGPL-3.0-or-later
// Live MIDI input: what is played on a MIDI keyboard or the computer keyboard goes to one track.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
// Tracktion's "All MIDI Ins" input: every enabled MIDI device merged, plus the notes the computer
// keyboard plays into it. It is always monitored, so the track it goes to plays what comes in.
// nullptr until Tracktion has scanned the MIDI devices, which it does just after the Engine starts.
std::shared_ptr<te::MidiInputDevice> getAllMidiInputs (te::Engine&);

// The track that live MIDI input plays, or nullptr.
te::AudioTrack* getLiveMidiTrack (te::Edit&);

// Makes live MIDI input play this track, and no other. Returns false if there is no MIDI input yet.
bool setLiveMidiTrack (te::AudioTrack&);

// If live MIDI input plays none of the Edit's tracks, sends it to the first track with an instrument.
// Also keeps the Edit's playback graph running, so that input is heard while the transport is
// stopped. Call it when a project opens and whenever the MIDI devices change.
void routeLiveMidiInput (te::Edit&);

// True if the track has an instrument (a synth or sampler plug-in) that live MIDI input can play.
bool hasInstrument (const te::AudioTrack&);

// The track the selection is about: the first selected audio track, or else the first selected clip's, or the
// track of the clip whose notes are selected.
te::AudioTrack* getSelectedAudioTrack (const te::SelectionManager&);

// Live MIDI input follows the selection, as FL Studio's selected channel does: if the selected track (see
// getSelectedAudioTrack()) has an instrument, it plays that track. Call it when the selection changes.
void followSelectedTrack (const te::SelectionManager&);
} // namespace phazite
