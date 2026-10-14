// SPDX-License-Identifier: AGPL-3.0-or-later
// Clips in the playlist: which are selected, the color they show, and the edits the Smart Tool makes.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
// The selected clips, top to bottom by track, then left to right. Sections aren't among them (Sections.h).
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

// Crossfades two audio clips that met at boundary on a track: each reaches over it by half the length, as
// far as its audio goes, and the first fades out as the second fades in. A length of 0 puts them back.
void setCrossfade (te::AudioClipBase& first, te::AudioClipBase& second, te::TimePosition boundary, te::TimeDuration length);

// Gives the audio clips among these fade-ins and fade-outs of a shape.
void setFadeShape (const juce::Array<te::Clip*>&, te::AudioFadeCurve::Type);

// Adds an empty MIDI clip, named after its track, and selects it if a selection manager is given.
te::MidiClip* insertMidiClip (te::ClipTrack&, te::BeatRange, te::SelectionManager*);

//==============================================================================
// Splits each clip that the beat falls inside into two there. Returns the new right-hand parts.
juce::Array<te::Clip*> splitClips (const juce::Array<te::Clip*>&, te::BeatPosition);

// Turns aliases back into separate clips. A clip left alone in its link group is unlinked too.
void makeClipsUnique (const juce::Array<te::Clip*>&);

// Mutes the clips, or unmutes them if they all are muted already.
void toggleClipsMuted (const juce::Array<te::Clip*>&);
bool areAllClipsMuted (const juce::Array<te::Clip*>&);

// Moves clips by an amount, all together, but none before the start of the song.
void nudgeClips (const juce::Array<te::Clip*>&, te::BeatDuration);

// Deletes clips. With closeGap, the clips after each one on its track move back by its length.
void deleteClips (const juce::Array<te::Clip*>&, bool closeGap);

// Deletes what lies in a time range on some tracks: clips inside it go, and clips across its edges are
// trimmed or split. With closeGap, what comes after on those tracks moves back to fill the gap. (Removing
// time from the whole song, sections and automation too, is Arrangement.h's removeTime().)
void deleteTimeRange (te::Edit&, te::BeatRange, const juce::Array<te::Track*>&, bool closeGap);
} // namespace phazite
