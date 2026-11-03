// SPDX-License-Identifier: AGPL-3.0-or-later
// Clips in the playlist: which are selected, the color they show, and the edits the Smart Tool makes. The song holds its
// clips in Phazite's own record of it (schema::CLIP, on their tracks' elements; docs/tracktion-migration.md, T1 step 3):
// where each is, in beats, its name, mute and alias group, and a MIDI clip's notes (model/notes/internal/Notes.h) or an
// audio clip's part of its file (AudioClips.h). Tracktion's clips mirror them (model/mirror/internal/Mirror.h), with the
// same IDs.
#pragma once

#include "model/arrangement/PlaylistValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The song's clips as Phazite's record holds them.

// A clip's element: invalid for a clip that isn't the song's (gone).
juce::ValueTree findClipState (const Song&, ClipID);
juce::ValueTree findClipState (const te::Clip&);
inline juce::ValueTree findClipState (const te::Edit& edit, ClipID id)
{
    return findClipState (getSong (edit), id);
}

// The clips' elements on a track's element; and on every track of the song, track by track.
std::vector<juce::ValueTree> getClipStates (const juce::ValueTree& trackState);
std::vector<juce::ValueTree> getClipStates (const Song&);
inline std::vector<juce::ValueTree> getClipStates (const te::Edit& edit)
{
    return getClipStates (getSong (edit));
}

ClipID getClipID (const juce::ValueTree& clipState);
BeatDuration getClipOffset (const juce::ValueTree& clipState);
BeatPosition getContentStart (const juce::ValueTree& clipState); // where its notes count from: its start, less its offset

// Where a clip is: a MIDI clip's as its element holds it, and any clip's, an audio clip's for as long as the part of its file
// it plays takes at the tempo there.
BeatRange getClipRange (const juce::ValueTree& clipState);
BeatRange getClipRange (const te::Edit&, const juce::ValueTree& clipState);

// Moves a clip to start at a beat, playing what it did (in the caller's undo step), and Tracktion's clip follows.
void moveClipState (te::Edit&, juce::ValueTree clipState, BeatPosition start);

// The clips of a clip's alias group, which hold the same notes (schema::clipLink), the clip itself first; only the clip
// if it is in none.
std::vector<juce::ValueTree> getAliasStates (const juce::ValueTree& clipState);

// A new MIDI clip's element, with an ID from the Edit's (as Tracktion's items get theirs while T1 lasts), on no track yet.
juce::ValueTree makeMidiClipState (Song&, const juce::String& name, BeatRange);

// A copy of a clip's element, on no track yet, with an ID of its own and the same notes, with the same IDs, in the same alias
// group.
juce::ValueTree copyClipState (Song&, const juce::ValueTree& clipState);

// Puts a clip's element on a track's, or moves it there, and has Tracktion's clip follow (in the caller's undo step).
// Returns Tracktion's clip.
te::Clip* placeClipState (te::Edit&, juce::ValueTree clipState, te::ClipTrack&);

// Where a clip is, in the caller's undo step, and has Tracktion's clip follow: its start, its length, and how far into
// its notes it starts.
void setClipPlace (te::Edit&, juce::ValueTree clipState, BeatPosition start, BeatDuration length, BeatDuration offset);

// A controller event of a MIDI clip of the song's (schema::CONTROL) as Tracktion's MidiControllerEvent has it: its type (a
// MIDI controller's number, or one of Tracktion's own types), its value (7 bits up, but for the pitch wheel's) and its
// metadata (an aftertouch's note). And the song's element for one of Tracktion's: invalid for a kind the song doesn't keep.
struct TracktionControl
{
    te::BeatPosition beat;
    int type = 0, value = 0, metadata = 0;
};

TracktionControl toTracktionControl (const juce::ValueTree& control);
juce::ValueTree makeControlState (const te::MidiControllerEvent&);

// Whether Tracktion's clip holds the controller events of the song's element for it, in their order.
bool haveSameControls (const te::MidiClip&, const juce::ValueTree& clipState);

// Takes one of Tracktion's MIDI clips on one of the song's tracks into the song, which hasn't it yet (one Tracktion made
// from a MIDI file): an element for it, with its notes, each with an ID of its own, and its controller events. Returns the
// element.
juce::ValueTree adoptMidiClip (te::MidiClip&);

//==============================================================================
// The selected clips, top to bottom by track, then left to right. Sections aren't among them (Sections.h). And Tracktion's
// clips for IDs, those still there, while the model's callers hold Tracktion's.
std::vector<ClipID> getSelectedClips (const Selection&);
bool hasSelectedClips (const Selection&);
juce::Array<te::Clip*> findClips (const te::Edit&, const std::vector<ClipID>&);
juce::Array<te::Clip*> findSelectedClips (const Selection&); // Tracktion's, for the selected clips

bool hasAliases (const Song&, const std::vector<ClipID>&);    // whether any of the clips is linked to others
bool hasAudioClips (const Song&, const std::vector<ClipID>&); // whether any of them holds audio

// Whether there is an audio clip among these, and every audio clip among them fades in and out in this shape. MIDI
// clips don't count.
bool hasFadeShape (const Song&, const std::vector<ClipID>&, FadeCurve);

// A copy of each clip, on the same track and in the same place, for the caller to move. As aliases,
// the copies stay linked to their originals: editing the notes of one edits them all.
juce::Array<te::Clip*> copyClips (const juce::Array<te::Clip*>&, bool asAliases);

// While a clip linked to one of these (an alias, which shares its notes) plays at a song beat, the song beat in that
// one whose notes play then: where an editor showing these clips shows that their notes are playing, as FL Studio's
// piano roll shows where its pattern is wherever the pattern plays. Nothing while one of these plays there itself (the
// playhead is there already), or no clip linked to them does.
std::optional<double> getLinkedPlayBeat (const juce::Array<te::MidiClip*>& shown, double songBeat);

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

// Gives the audio clips among these fade-ins and fade-outs of a shape (PlaylistValues.h: FadeCurve); and one of an audio
// clip's fades a shape, as one undo step ("Change Fade Shape").
void setFadeShape (const juce::Array<te::Clip*>&, FadeCurve);
void setFadeCurve (te::Clip&, bool fadeIn, FadeCurve);

// Adds an empty MIDI clip, named after its track or as given, and selects it if a selection manager is given.
te::MidiClip* insertMidiClip (te::ClipTrack&, te::BeatRange, Selection*, const juce::String& name = {});

// The same, a bar long (in the time signature there) from a beat.
te::MidiClip* insertBarLongMidiClip (te::ClipTrack&, te::BeatPosition start, Selection*);

// The same, as one undo step, "Add MIDI Clip": a double-click on a track with an instrument.
te::MidiClip* addMidiClipAt (te::ClipTrack&, te::BeatPosition, Selection*);

// What the Draw and Paint tools place at a beat on a track: a copy of the clip used last (of a MIDI clip, an alias that
// stays linked to it, as FL Studio places the same pattern again), or, before any clip was used or once it is gone, an
// empty MIDI clip a bar long on a track with an instrument. Returns it, or nullptr when there is nothing to
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
// the clips span, on its track, and the automation under each is copied with it (model/automation/internal/
// Automation.h: moveAutomation()). As aliases, the copies stay linked to their originals. With a selection manager, the
// copies are selected instead of what was. Returns the copies.
juce::Array<te::Clip*> duplicateClips (const juce::Array<te::Clip*>&, bool asAliases, Selection*);

// Turns aliases back into separate clips. A clip left alone in its link group is unlinked too.
void makeClipsUnique (Song&, const std::vector<ClipID>&);

// Mutes the clips, or unmutes them if they all are muted already.
void toggleClipsMuted (Song&, const std::vector<ClipID>&);
bool areAllClipsMuted (const Song&, const std::vector<ClipID>&);

// Moves clips by an amount, all together, but none before the start of the song, with the automation under them.
void nudgeClips (const juce::Array<te::Clip*>&, te::BeatDuration);

// Deletes clips, leaving the automation under them. With closeGap, each one's time goes from its track, with the
// automation in it, and what comes after moves back by its length.
void deleteClips (const juce::Array<te::Clip*>&, bool closeGap);

// Deletes what lies in a time range on some tracks: clips inside it go, clips across its edges are trimmed or split,
// and the tracks' automation in it goes. With closeGap, what comes after on those tracks moves back to fill the gap.
// (Removing time from the whole song, sections too, is Arrangement.h's removeTime().)
void deleteTimeRange (te::Edit&, te::BeatRange, const juce::Array<te::Track*>&, bool closeGap);

// The same for a track's clips alone, without the automation there or closing the gap: what Paste lands on (Clipboard.h).
void clearClipsIn (te::ClipTrack&, te::BeatRange);

// Copies what lies in a time range on some tracks right after it, on the same tracks, and returns where the copy is.
// The copy is cut at the range's edges, as Split cuts clips; the clips themselves aren't cut there. Each piece of the
// copy replaces what lies under it (as REAPER's razor edits trim what is behind them), so nothing plays twice, and what
// no piece lands on stays. The pieces are copies, not aliases. The tracks' automation in the range is copied too, in
// place of theirs where it lands. (Copying a range on every track, with room made for it, is Arrangement.h's
// copyTime().)
te::BeatRange duplicateTimeRange (te::Edit&, te::BeatRange, const juce::Array<te::Track*>&);
} // namespace phazite
