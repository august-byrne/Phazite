// SPDX-License-Identifier: AGPL-3.0-or-later
// Clips in the playlist: which are selected, the color they show, and the edits the Smart Tool makes. The song holds its
// clips (schema::CLIP, on their tracks' elements): where each is, in beats, its name, mute and alias group, and a MIDI
// clip's notes (model/notes/internal/Notes.h) or an audio clip's part of its file (AudioClips.h), by their IDs.
#pragma once

#include "model/arrangement/PlaylistValues.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The song's clips' elements.

// A clip's element: invalid for a clip that isn't the song's (gone).
juce::ValueTree findClipState (const Song&, ClipID);

// The clips' elements on a track's element; and on every track of the song, track by track. And a track's clips' IDs, in
// the order its element holds them, and the track a clip is on (none for a clip that is gone).
std::vector<juce::ValueTree> getClipStates (const juce::ValueTree& trackState);
std::vector<juce::ValueTree> getClipStates (const Song&);
std::vector<ClipID> getClips (const Song&, TrackID);
TrackID getClipTrack (const Song&, ClipID);

ClipID getClipID (const juce::ValueTree& clipState);
BeatDuration getClipOffset (const juce::ValueTree& clipState);
BeatPosition getContentStart (const juce::ValueTree& clipState); // where its notes count from: its start, less its offset
bool isMidiState (const juce::ValueTree& clipState);

// Where a clip is: a MIDI clip's as its element holds it, and any clip's, an audio clip's for as long as the part of its file
// it plays takes at the tempo there.
BeatRange getClipRange (const juce::ValueTree& clipState);
BeatRange getClipRange (const Song&, const juce::ValueTree& clipState);
BeatRange getClipRange (const Song&, ClipID);

// Where the song's last clip ends, muted or not, in folders too; its start for a song without clips.
BeatPosition getClipsEnd (const Song&);

// Moves a clip to start at a beat, playing what it did (in the caller's undo step).
void moveClipState (Song&, juce::ValueTree clipState, BeatPosition start);

// The clips of a clip's alias group, which hold the same notes (schema::clipLink), the clip itself first; only the clip
// if it is in none.
std::vector<juce::ValueTree> getAliasStates (const juce::ValueTree& clipState);

// A new MIDI clip's element, with an ID of the song's, on no track yet.
juce::ValueTree makeMidiClipState (Song&, const juce::String& name, BeatRange);

// A copy of a clip's element, on no track yet, with an ID of its own and the same notes, with the same IDs, in the same alias
// group.
juce::ValueTree copyClipState (Song&, const juce::ValueTree& clipState);

// Puts a clip's element on a track's, or moves it there (in the caller's undo step). Returns its ID, or none if the track
// isn't there.
ClipID placeClipState (Song&, juce::ValueTree clipState, TrackID);

// Where a clip is, in the caller's undo step: its start, its length, and how far into its notes it starts.
void setClipPlace (Song&, juce::ValueTree clipState, BeatPosition start, BeatDuration length, BeatDuration offset);

//==============================================================================
// The selected clips, top to bottom by track, then left to right. Sections aren't among them (Sections.h).
std::vector<ClipID> getSelectedClips (const Selection&);
bool hasSelectedClips (const Selection&);

bool hasAliases (const Song&, const std::vector<ClipID>&);    // whether any of the clips is linked to others
bool hasAudioClips (const Song&, const std::vector<ClipID>&); // whether any of them holds audio

// Whether there is an audio clip among these, and every audio clip among them fades in and out in this shape. MIDI
// clips don't count.
bool hasFadeShape (const Song&, const std::vector<ClipID>&, FadeCurve);

// A copy of each clip, on the same track and in the same place, for the caller to move. As aliases,
// the copies stay linked to their originals: editing the notes of one edits them all.
std::vector<ClipID> copyClips (Song&, const std::vector<ClipID>&, bool asAliases);

// While a clip linked to one of these (an alias, which shares its notes) plays at a song beat, the song beat in that
// one whose notes play then: where an editor showing these clips shows that their notes are playing, as FL Studio's
// piano roll shows where its pattern is wherever the pattern plays. Nothing while one of these plays there itself (the
// playhead is there already), or no clip linked to them does.
std::optional<double> getLinkedPlayBeat (const Song&, const std::vector<ClipID>& shown, double songBeat);

// Moves a clip to start at a beat (not before the song does) on a track, keeping its length.
void moveClip (Song&, ClipID, BeatPosition start, TrackID);

// Moves a clip's start or end to a beat, leaving its content where it is. A clip can't start before its
// content does, an audio clip can't end after its audio, and every clip stays at least a 1/64 beat long.
void trimClipStart (Song&, ClipID, BeatPosition);
void trimClipEnd (Song&, ClipID, BeatPosition);

// Sets an audio clip's fade-in or fade-out, in seconds, at most as long as the clip.
void setClipFade (Song&, ClipID, bool fadeIn, double seconds);

// Crossfades two audio clips that met at a boundary on a track, in the song's seconds: each reaches over it by half the
// length, as far as its audio goes, and the first fades out as the second fades in. A length of 0 puts them back.
void setCrossfade (Song&, ClipID first, ClipID second, double boundarySeconds, double lengthSeconds);

// Gives the audio clips among these fade-ins and fade-outs of a shape (PlaylistValues.h: FadeCurve); and one of an audio
// clip's fades a shape, as one undo step ("Change Fade Shape").
void setFadeShape (Song&, const std::vector<ClipID>&, FadeCurve);
void setFadeCurve (Song&, ClipID, bool fadeIn, FadeCurve);

// Adds an empty MIDI clip, named after its track or as given, and selects it if a selection is given. Returns none if the
// track isn't there.
ClipID insertMidiClip (Song&, TrackID, BeatRange, Selection*, const juce::String& name = {});

// The same, a bar long (in the time signature there) from a beat.
ClipID insertBarLongMidiClip (Song&, TrackID, BeatPosition start, Selection*);

// The same, as one undo step, "Add MIDI Clip": a double-click on a track with an instrument.
ClipID addMidiClipAt (Song&, TrackID, BeatPosition, Selection*);

// What the Draw and Paint tools place at a beat on a track: a copy of the clip used last (of a MIDI clip, an alias that
// stays linked to it, as FL Studio places the same pattern again), or, before any clip was used or once it is gone, an
// empty MIDI clip a bar long on a track with an instrument. Returns it, or none when there is nothing to place there.
ClipID placeClip (Song&, TrackID, BeatPosition start, ClipID lastUsed);

// Slips what a clip plays, as the Slip tool does: its content moves later by a distance in seconds (earlier if negative),
// from the offset it had when the drag began, while the clip stays where it is. Content can't start after the clip does.
void slipClip (Song&, ClipID, double startOffsetSeconds, double distanceSeconds);

// How far into what it plays a clip starts, in seconds (an audio clip's into its file as it plays it, a MIDI clip's into
// its notes at the song's tempo): what the Slip tool starts from.
double getClipStartOffset (const Song&, ClipID);

//==============================================================================
// Splits each clip that the beat falls inside into two there. Returns the new right-hand parts.
std::vector<ClipID> splitClips (Song&, const std::vector<ClipID>&, BeatPosition);

// Splits the clips on these tracks at both edges of a time range.
void splitClipsAtEdges (Song&, const std::vector<TrackID>&, BeatRange);

// Copies the clips right after them all, as FL Studio's Ctrl+B does: each copy starts as far after its original as
// the clips span, on its track, and the automation under each is copied with it (model/automation/internal/
// Automation.h: moveAutomation()). As aliases, the copies stay linked to their originals. With a selection, the copies are
// selected instead of what was. Returns the copies.
std::vector<ClipID> duplicateClips (Song&, const std::vector<ClipID>&, bool asAliases, Selection*);

// Turns aliases back into separate clips. A clip left alone in its link group is unlinked too.
void makeClipsUnique (Song&, const std::vector<ClipID>&);

// Mutes the clips, or unmutes them if they all are muted already.
void toggleClipsMuted (Song&, const std::vector<ClipID>&);
bool areAllClipsMuted (const Song&, const std::vector<ClipID>&);

// Moves clips by an amount, all together, but none before the start of the song, with the automation under them.
void nudgeClips (Song&, const std::vector<ClipID>&, BeatDuration);

// Deletes clips, leaving the automation under them. With closeGap, each one's time goes from its track, with the
// automation in it, and what comes after moves back by its length.
void deleteClips (Song&, const std::vector<ClipID>&, bool closeGap);

// Deletes what lies in a time range on some tracks: clips inside it go, clips across its edges are trimmed or split,
// and the tracks' automation in it goes. With closeGap, what comes after on those tracks moves back to fill the gap.
// (Removing time from the whole song, sections too, is Arrangement.h's removeTime().)
void deleteTimeRange (Song&, BeatRange, const std::vector<TrackID>&, bool closeGap);

// The same for a track's clips alone, without the automation there or closing the gap: what Paste lands on (Clipboard.h).
void clearClipsIn (Song&, TrackID, BeatRange);

// Copies what lies in a time range on some tracks right after it, on the same tracks, and returns where the copy is.
// The copy is cut at the range's edges, as Split cuts clips; the clips themselves aren't cut there. Each piece of the
// copy replaces what lies under it (as REAPER's razor edits trim what is behind them), so nothing plays twice, and what
// no piece lands on stays. The pieces are copies, not aliases. The tracks' automation in the range is copied too, in
// place of theirs where it lands. (Copying a range on every track, with room made for it, is Arrangement.h's
// copyTime().)
BeatRange duplicateTimeRange (Song&, BeatRange, const std::vector<TrackID>&);
} // namespace phazite
