// SPDX-License-Identifier: AGPL-3.0-or-later
// Clips in the playlist: which are selected, the color they show, and the edits the Smart Tool makes. The song holds its
// clips as values, in its clips' area (model/base/internal/ClipsValue.h: SongClips), each track's in their
// order: where each is, in beats, its name, mute and alias group, and a MIDI clip's notes (model/notes/internal/Notes.h) or
// an audio clip's part of its file (AudioClips.h), by their IDs.
#pragma once

#include "model/arrangement/PlaylistValues.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SongValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The song's clips by their IDs, as plain values (model/base/internal/ClipsValue.h: ClipValue), read from the song's clips'
// area: a lookup in a map, a copy of a small value. What reads many clips often without a copy finds them in the area itself
// (SongValues::findClip()).

// A clip's value: none for a clip that isn't the song's (gone).
std::optional<ClipValue> getClipValue (const Song&, ClipID);

// A track's clips' values, in the order the track holds them (none for a track that isn't there, or holds no clips); and
// every clip's of the song, track by track.
std::vector<ClipValue> getClipValues (const Song&, TrackID);
std::vector<ClipValue> getClipValues (const Song&);

// Every clip of the song, track by track, each track's in its order.
std::vector<ClipID> getAllClips (const Song&);

// Whether a track holds any clip; any clip of a kind.
bool hasClips (const Song&, TrackID);
bool hasClips (const Song&, TrackID, ClipValue::Kind);

// Whether a clip is a MIDI clip of the song's (an audio clip's: AudioClips.h, isAudioClip()).
bool isMidiClip (const Song&, ClipID);

// Where a MIDI clip's notes count from: its start, less its offset.
BeatPosition getContentStart (const ClipValue&);
BeatPosition getContentStart (const Song&, ClipID);

// Where a clip is: a MIDI clip's as its value holds it (an audio clip's start alone: its length follows from its part).
BeatRange getClipRange (const ClipValue&);

// Where a clip is in the song: a MIDI clip's as its value holds it, an audio clip's for as long as the part of its file it
// plays takes at the song's tempo there (AudioClips.h: getAudioClipRange()), worked out from the value alone: the song's, or
// one an edit is to give it.
BeatRange getLiveClipRange (const Song&, const ClipValue&);

// Where a clip is on its track, without the rest of its value: its ID, its kind, its start, and a MIDI clip's length and
// offset (an audio clip's length is 0, as its value's is), for what walks many clips often. A track's clips', in its order,
// and every clip's of the song, track by track; and where one is in the song, an audio clip's read from it by its ID.
struct ClipPlacement
{
    ClipID id;
    ClipValue::Kind kind = ClipValue::Kind::midi;
    BeatPosition start;
    BeatDuration length, offset;

    bool isAudio() const noexcept { return kind == ClipValue::Kind::audio; }
};

std::vector<ClipPlacement> getClipPlacements (const Song&, TrackID);
std::vector<ClipPlacement> getClipPlacements (const Song&);
BeatRange getClipRange (const Song&, const ClipPlacement&);

// The clips of a clip's alias group (Schema.h: clipLink), the clip itself first, then the others in the song's order; only
// the clip if it is in none, and none for a clip that isn't the song's.
std::vector<ClipID> getAliasClips (const Song&, ClipID);

// A clip's name: its own, or else its track's, whatever the track is called now.
juce::String getClipName (const Song&, ClipID);
juce::String getClipName (const Song&, const ClipValue&);

//==============================================================================
// The song's clips changed by their IDs and plain values: how anything writes a clip, the clips' own edits too, each a new
// value of the song's clips' area (SongValues.h: SongValues::setClip()). None begins an undo step: each change joins the
// caller's, or is made outside the history while the song is written outside it (Song::OutsideHistory). What else changes
// clips by ID is below: moveClip(), deleteClips(), setClipFade() and the rest, and AudioClips.h's, each worked out on the
// clips' values and set through these.

// A new MIDI clip's value, with an ID of the song's, on no track yet (its track is the caller's to give): from the range's
// start (not before the song's), at least shortestClipBeats long. With no name, it has none of its own: it is called after
// the track it goes on (getClipName()).
ClipValue makeMidiClip (Song&, const juce::String& name, BeatRange);

// Adds a clip to the song, on the track its value names, after that track's clips (or at a place among them, from 0), with
// its notes and controllers' values where it is a MIDI clip and the caller has them (none: an empty clip's, or its alias
// group's where it joins one). It takes the ID its value carries if no clip of the song has it, else a new one: a clip made
// by makeMidiClip() or AudioClips.h's makeAudioClip() keeps its own; one given no ID (a paste) gets one. Returns its ID, or
// none if the track isn't there or holds no clips (a folder or a return: Schema.h, trackKind), where nothing changes. What
// may go on the track is the caller's to ask (canTrackHold()).
// It doesn't ask canTrackHold() itself: the DAWproject import puts what a file's track holds on one track (known bug 235,
// docs/known-bugs.md); every other caller asks before it calls.
ClipID addClip (Song&, ClipValue, const ClipNotes* notes = nullptr, int index = -1);

// Gives a clip of the song (by its value's ID) the value's properties, kept within what a clip holds (ClipsValue.h:
// keepWithinClip()), changing only what differs from what it holds: on another track, it moves to the end of that one's
// clips (one that holds no clips isn't one it goes on). A MIDI clip that joins an alias group takes the group's notes; one
// that leaves keeps those it holds. Nothing for a clip that isn't the song's.
void setClip (Song&, const ClipValue&);

// The same for a clip whose value the caller read (getClipValue()) and holds as the song holds it: what the clips' own edits
// set (Clips.cpp, AudioClips.cpp), each having worked the next value out by its own rules (its start, length and part kept
// within what a clip holds as each edit keeps them, which setClip() above does for what is given from outside). Nothing
// for a next value the same as the one held; on another track, it moves to the end of that one's clips.
void setClip (Song&, const ClipValue& held, const ClipValue& next);

// Takes a clip out of the song, its notes with it. The last clip left in its alias group leaves the group too (a group of one
// would still show as linked to nothing), as Make Unique leaves it.
void eraseClip (Song&, ClipID);

// What a track's removal takes of the clips (Tracks.h: eraseTrackHoldings()): every clip of these tracks, with its notes, in
// one set of each area (SongValues.h: eraseClips()), each track's list looked in alone. An alias group left with one clip
// keeps it linked: the remover unlinks it after (unlinkLoneClips()).
void eraseTrackClips (Song&, const std::vector<TrackID>&);

// The clips a track's element holds (its CLIP children, not those of the tracks in it) given to the song's track of its ID (a
// merge's choice of a whole track: Tracks.h, chooseTrackClash()): each read as the song holds a clip (readClipValue(),
// keepWithinClip()), in their order, after the track's others; a MIDI clip with the notes of the alias group it joins where
// another clip of it holds some, else its own; an audio clip with the sound of its group's where it holds one alike; one of an ID
// the song holds on another track moved here. Nothing for a track that holds no clips (a folder's, a return's).
void takeClipsOf (Song&, const juce::ValueTree& track);

// The value a clip of the song would hold were setClip() to give it this one, changing nothing: what a clip takes of it
// (ClipsValue.h, keepWithinClip(): a start at 0 or later, a MIDI clip's length at least shortestClipBeats, a file's part
// ending at its start or later; nothing of the other kind of clip's properties), as getClipValue() would read it after.
// None for a clip that isn't the song's.
std::optional<ClipValue> getClipValueAsSet (const Song&, const ClipValue&);

// Puts a clip in an alias group (Schema.h: clipLink), or in none (0), as setClip() does; the other clips of a group it
// leaves stay in it, the caller's to take out where one is left alone (unlinkLoneClips()).
void setClipLink (Song&, ClipID, std::uint64_t link);

// Gives every audio clip that plays a file by one path (as the song holds it: Schema.h, clipFile) another, as Locate Audio
// File does (model/project/ProjectContents.h). Whether any played it.
bool setClipFiles (Song&, const juce::String& path, const juce::String& newPath);

// Copies a track's clips onto another (a copy of it, as Duplicate makes one: model/arrangement/internal/Tracks.h,
// duplicateTracks()): each copy where its clip is in the song, added after the clips the other holds (a fresh copy holds
// none), in the track's order, with the ID the copy gave it (ids: an old ID to its new one, as Schema.h's giveNewIDs() gives
// them; a new one for a clip they don't name) and its notes, shared until either is edited; as aliases (each copy in its
// clip's alias group), or each in none.
void copyTrackClips (Song&, TrackID from, TrackID to, const std::map<std::uint64_t, std::uint64_t>& ids, bool asAliases);

// Puts a track's clips in another order (the order getClips() gives): the same clips, each once, else nothing changes. What
// makes a track's clips like another song's (model/versions/internal/ApplySong.h) puts them in its order with.
void setTrackClipOrder (Song&, TrackID, const std::vector<ClipID>& order);

// Moves a clip to start at a beat (not before the song does), on its track, playing what it did.
void moveClipStart (Song&, ClipID, BeatPosition start);

// Where a MIDI clip is: its start (not before the song's), its length (at least shortestClipBeats) and how far into its notes
// it starts (from 0).
void setClipPlace (Song&, ClipID, BeatPosition start, BeatDuration length, BeatDuration offset);

//==============================================================================
// A track's clips' IDs, in its order, and the track a clip is on (none for a clip that is gone).
std::vector<ClipID> getClips (const Song&, TrackID);
TrackID getClipTrack (const Song&, ClipID);

// Where a clip is: a MIDI clip's as its value holds it, an audio clip's for as long as the part of its file it plays takes at
// the tempo there (getLiveClipRange()); nothing for a clip that isn't the song's.
BeatRange getClipRange (const Song&, ClipID);

// Where the song's last clip ends, muted or not, in folders too; its start for a song without clips.
BeatPosition getClipsEnd (const Song&);

// A step of its own, "Rename Clip" ("Rename Clips" for several): the clips, and the aliases of each, which play what it plays,
// take the name. A name that is empty, or a clip's track's, leaves the clip without one of its own, called after its track.
void renameClips (Song&, const std::vector<ClipID>&, const juce::String& name);

// Whether a track can hold clips of a kind, MIDI or audio: audio and notes don't share a track (docs/design/building-blocks.md).
// A track that holds clips (not a folder or a return), whose clips, but for those named in leaving, are all of that kind; and
// for audio, one without an instrument, which would silence it.
bool canTrackHold (const Song&, TrackID, bool midi, const std::vector<ClipID>& leaving = {});

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

// Moves a clip to start at a beat (not before the song does) on a track, keeping its length. Onto another track only if
// that track can hold it (canTrackHold()); else nothing changes.
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
// from the offset it had when the drag began, while the clip stays where it is. Content can't start after the clip does,
// and an audio clip's part stays inside its file, as long as it is, whichever way it plays.
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

// Unlinks every clip left alone in its link group (after its tracks went, say), in the caller's undo step. Deleting a
// clip unlinks the last one of its group itself.
void unlinkLoneClips (Song&);

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
