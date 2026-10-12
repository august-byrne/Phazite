// SPDX-License-Identifier: AGPL-3.0-or-later
// Clips in the playlist: which are selected, the color they show, and the edits the Smart Tool makes.
#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace phazite
{
namespace te = tracktion;

// The selected clips, top to bottom by track, then left to right.
juce::Array<te::Clip*> getSelectedClips (const te::SelectionManager&);

// The color a clip shows: its own, if it was given one, otherwise its track's.
juce::Colour getClipColour (const te::Clip&, juce::Colour trackColour);

// A copy of each clip, on the same track and in the same place, for the caller to move. As aliases,
// the copies stay linked to their originals: editing the notes of one edits them all.
juce::Array<te::Clip*> copyClips (const juce::Array<te::Clip*>&, bool asAliases);

// Moves a clip to start at a beat (not before the song does) on a track, keeping its length.
void moveClip (te::Clip&, te::BeatPosition start, te::ClipTrack&);

// Moves a clip's start or end to a beat, leaving its content where it is. A clip can't start before its
// content does, an audio clip can't end after its audio, and every clip stays at least a 1/64 beat long.
void trimClipStart (te::Clip&, te::BeatPosition);
void trimClipEnd (te::Clip&, te::BeatPosition);

// Sets an audio clip's fade-in or fade-out, at most as long as the clip.
void setClipFade (te::AudioClipBase&, bool fadeIn, te::TimeDuration);

// Adds an empty MIDI clip, named after its track, and selects it if a selection manager is given.
te::MidiClip* insertMidiClip (te::ClipTrack&, te::BeatRange, te::SelectionManager*);
} // namespace phazite
