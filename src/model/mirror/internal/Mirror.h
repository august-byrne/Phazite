// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion's Edit as a mirror of Phazite's own record of the song (docs/tracktion-migration.md, T1): what that document
// holds, written into Tracktion's state so that Tracktion plays it. One way: Tracktion never writes into the document, and
// no model function reads Tracktion's state as the truth of what the document holds.
//
// While T1 moves the song into the document part by part, the document is Phazite's state in the Edit (getSong(),
// model/base/internal/Schema.h), and one undo manager, the Edit's, holds both. The model's functions change the document,
// then call a part's reconcile function: it writes into Tracktion's state what differs from the document, with the Edit's
// undo manager, so that an undo or a redo puts both back together and the mirror has nothing to do then. Each is
// idempotent: called when nothing differs, it changes nothing. Message thread only.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"

#include <vector>

namespace phazite::mirror
{
// Tracktion's track for one of the song's, by its ID or its element, for the model's functions that still take Tracktion's
// tracks while T6 lasts (docs/tracktion-migration.md): nullptr for none.
te::Track* findTrack (const te::Edit&, TrackID);
te::Track* findTrack (const te::Edit&, const juce::ValueTree& trackState);
juce::Array<te::Track*> findTracks (const te::Edit&, const std::vector<TrackID>&); // those there, in their order

// A file Tracktion's clips may play, which Tracktion may have read before: read again (a render made since it last looked),
// or let go of before it is deleted.
void reconcileFile (te::Edit&, const juce::File&);
void releaseFile (te::Edit&, const juce::File&);

// The tempo map (model/transport/internal/Tempo.h): Tracktion's tempo sequence gets the document's tempos and time
// signatures, each at its beat; and the clips and the automation are placed again for them (reconcileClips(),
// reconcileAutomation()).
void reconcileTempoMap (te::Edit&);

// The tracks (model/arrangement/internal/Tracks.h): Tracktion gets a track for each of the document's, with its ID, in
// its place and folder, with its own devices as the document has them (its fader, with the document's ID, and its meter,
// or a plain folder's VCA: one of the other kind or with another ID is made again), and the mute, solo and solo isolation
// (a return's) that it plays with, and its write mode (the master's too); Tracktion's tracks that the document hasn't go,
// with what they hold. Nothing for a song without a track list (an Edit that isn't a song's).
void reconcileTracks (te::Edit&);

// The clips (model/arrangement/internal/Clips.h): Tracktion gets a clip for each of the song's, with its ID, on its track,
// where it is in seconds (from its beats, as the tempo map has them now), with its name and its mute, in no alias group of
// Tracktion's (the song keeps its aliases the same itself, so Tracktion's copying between them never runs): a MIDI clip
// with its notes, each with its ID (schema::phaziteNoteID), and its controller events; an audio clip playing its file, or
// Phazite's render of it, from where the part it plays starts, with its gain and its fades, fitted to it as Tracktion would
// fit them (AudioClips.h). Tracktion's clips on the song's tracks that the song hasn't go. The tempo map's reconcile calls
// it, since where a clip is in seconds, and how a clip that follows the tempo is stretched, follow the tempo.
void reconcileClips (te::Edit&);

// The same for one clip, after a change to it alone (its notes, its place), or to its going.
void reconcileClip (te::Edit&, ClipID);

// The automation (model/automation/internal/Automation.h): the curve of each device's parameter gets the song's points
// for it, each in seconds from its beat, as the tempo map has them now, with its value and its curve; Tracktion's curves
// that the song hasn't are emptied. Not those of devices the song keeps curves for while an undo can bring them back.
void reconcileAutomation (te::Edit&);

// The same for one parameter's curve, after a change to it alone.
void reconcileCurve (te::Edit&, const ParameterRef&);

// The devices (model/mixer/internal/Mixer.h): each track's and the master's devices in Tracktion are the song's, in its
// order, each set as the song says, and its containers' devices in their rack types; Tracktion's devices that the song
// hasn't on that track go (a container with what it still holds, its rack type kept: model/mixer/internal/Containers.h),
// and those it has and Tracktion hasn't are made, with the tracks they listen to through their sidechains. Each channel's
// fader plays the song's volume and pan, its sends are the song's, and each return has its input. The modulation follows
// (reconcileModulation()), and last the frozen tracks: each plays its frozen file, if it is in the Edit's temporary
// folder, or live. The model still makes a device in Tracktion as it adds it to the song.
void reconcileDevices (te::Edit&);

// The state Tracktion makes an Edit built from a song alone from (createEditFromSong(), model/project/internal/Project.h):
// the song in it from the start, with its count of IDs where Tracktion counts from, so that what Tracktion makes as it
// makes the Edit takes none of the song's IDs, and the master's fader, which Tracktion makes with the Edit, by the song's
// ID (Schema.h: channelFader). The rest the reconcile functions make.
juce::ValueTree makeEditState (const juce::ValueTree& song);

// The modulation (model/modulation/internal/Modulation.h): each track's and the master's modulators in Tracktion's
// modifier lists, and each track's and container's macros, are the song's, in its order, each set as it has it (made, or
// taken away with what it modulates); and what each modulates, by its amount: Tracktion's assignments, of which those of a
// modulator or macro the song hasn't go.
void reconcileModulation (te::Edit&);

// Only the switches Tracktion keeps outside its undo history (the tracks' mute, solo and solo isolation), and the write
// modes, which Phazite sets outside it: after a write mode changed.
void reconcileTrackSwitches (te::Edit&);

// A parameter's value set by hand as the song has it, which Tracktion plays where nothing else moves it (a curve, a
// modulator): how the mirror sets one. Tracktion moves a curve's one point to the value as it is set, as for a control
// turned, which the song takes in then (model/automation/internal/Automation.h: adoptMovedPoint()); set by the mirror, it
// is no change of the song's (isSettingValues()), and the song's curve plays again after.
void setValueSetByHand (te::AutomatableParameter&, float value);
bool isSettingValues();

// Everything (T1, step 11): after the song's history took a step back or forth (phazite::undo() and redo(),
// model/commands/internal/Undo.h), which changes the song alone, Tracktion's Edit is brought in line with it, part by part:
// the tracks, the tempo map, the clips, the devices with the modulation and the frozen tracks, the automation and the
// transport. Records nothing in the song's history, so its redo history stays.
void reconcileAll (te::Edit&);

// A part of the song (Song::reconcile(), which the song calls this through after a change to it).
void reconcile (te::Edit&, Song::Part, std::uint64_t id = 0);

// The transport's state that the song keeps (model/transport/internal/Playback.h, Metronome.h), which Tracktion keeps
// outside its undo history: its transport loops, around the song or the range looped, starts playback where the song
// says, each in seconds from its beats as the tempo map has them now, and its click track clicks. After each change to
// them, to the tempo map, to the song's end, and after an undo or a redo, which can change the tempo map; begins no undo
// step. Not while Tracktion records, which loops as the take needs (model/transport/internal/Recording.h).
void reconcileTransport (te::Edit&);
} // namespace phazite::mirror
