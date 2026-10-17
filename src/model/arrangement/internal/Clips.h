// SPDX-License-Identifier: AGPL-3.0-or-later
// Clips in the playlist: which are selected, the color they show, and the edits the Smart Tool makes.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <optional>

namespace phazite
{
// The selected clips, top to bottom by track, then left to right. Sections aren't among them (Sections.h).
juce::Array<te::Clip*> getSelectedClips (const te::SelectionManager&);
bool hasSelectedClips (const te::SelectionManager&);

bool hasAliases (const juce::Array<te::Clip*>&);    // whether any of the clips is linked to others
bool hasAudioClips (const juce::Array<te::Clip*>&); // whether any of them holds audio

// Whether there is an audio clip among these, and every audio clip among them fades in and out in this shape. MIDI
// clips don't count.
bool hasFadeShape (const juce::Array<te::Clip*>&, te::AudioFadeCurve::Type);

// A copy of each clip, on the same track and in the same place, for the caller to move. As aliases,
// the copies stay linked to their originals: editing the notes of one edits them all.
juce::Array<te::Clip*> copyClips (const juce::Array<te::Clip*>&, bool asAliases);

// While a clip linked to one of these (an alias, which shares its notes) plays at a song beat, the song beat in that
// one whose notes play then: where an editor showing these clips shows that their notes are playing, as FL Studio's
// piano roll shows where its pattern is wherever the pattern plays. Nothing while one of these plays there itself (the
// playhead is there already), or no clip linked to them does.
std::optional<double> getLinkedPlayBeat (const juce::Array<te::MidiClip*>& shown, double songBeat);

// Moves a clip to start at a beat (not before the song does) on a track, keeping its length. An automation clip, copied,
// split or moved, sets what it set (model/automation/internal/Automation.h: AutomationTargets).
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

// The same, a bar long (in the time signature there) from a beat.
te::MidiClip* insertBarLongMidiClip (te::ClipTrack&, te::BeatPosition start, te::SelectionManager*);

// The same, as one undo step, "Add MIDI Clip": a double-click on a track with an instrument.
te::MidiClip* addMidiClipAt (te::ClipTrack&, te::BeatPosition, te::SelectionManager*);

// What the Draw and Paint tools place at a beat on a track: a copy of the clip used last (of a MIDI or automation clip,
// an alias that stays linked to it, as FL Studio places the same pattern again), or, before any clip was used or once it
// is gone, an empty MIDI clip a bar long on a track with an instrument. Returns it, or nullptr when there is nothing to
// place there.
te::Clip* placeClip (te::ClipTrack&, te::BeatPosition start, te::Clip* lastUsed);

// Slips what a clip plays, as the Slip tool does: its content moves later by a distance (earlier if negative), from the
// offset it had when the drag began, while the clip stays where it is. Content can't start after the clip does.
void slipClip (te::Clip&, te::TimeDuration startOffset, te::TimeDuration distance);

//==============================================================================
// Splits each clip that the beat falls inside into two there. Returns the new right-hand parts.
juce::Array<te::Clip*> splitClips (const juce::Array<te::Clip*>&, te::BeatPosition);

// Splits the clips on these tracks at both edges of a time range.
void splitClipsAtEdges (const juce::Array<te::Track*>&, te::BeatRange);

// Copies the clips right after them all, as FL Studio's Ctrl+B does: each copy starts as far after its original as
// the clips span, on its track. As aliases, the copies stay linked to their originals. With a selection manager, the
// copies are selected instead of what was. Returns the copies.
juce::Array<te::Clip*> duplicateClips (const juce::Array<te::Clip*>&, bool asAliases, te::SelectionManager*);

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

// Copies what lies in a time range on some tracks right after it, on the same tracks, and returns where the copy is.
// The copy is cut at the range's edges, as Split cuts clips; the clips themselves aren't cut there. Each piece of the
// copy replaces what lies under it (as REAPER's razor edits trim what is behind them), so nothing plays twice, and what
// no piece lands on stays. The pieces are copies, not aliases. (Copying a range on every track, with room made for it,
// is Arrangement.h's copyTime().)
te::BeatRange duplicateTimeRange (te::Edit&, te::BeatRange, const juce::Array<te::Track*>&);
} // namespace phazite
