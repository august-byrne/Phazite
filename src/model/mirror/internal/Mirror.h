// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion's Edit as a mirror of Phazite's own record of the song (docs/tracktion-migration.md, T1): what that document
// holds, written into Tracktion's state so that Tracktion plays it. One way: Tracktion never writes into the document, and
// no model function reads Tracktion's state as the truth of what the document holds.
//
// While T1 moves the song into the document part by part, the document is Phazite's state in the Edit (getPhaziteState(),
// model/base/internal/Schema.h), and one undo manager, the Edit's, holds both. The model's functions change the document,
// then call a part's reconcile function: it writes into Tracktion's state what differs from the document, with the Edit's
// undo manager, so that an undo or a redo puts both back together and the mirror has nothing to do then. Each is
// idempotent: called when nothing differs, it changes nothing. Message thread only.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"

namespace phazite::mirror
{
// The tempo map (model/transport/internal/Tempo.h): Tracktion's tempo sequence gets the document's tempos and time
// signatures, each at its beat; and the clips and the automation are placed again for them (reconcileClips(),
// reconcileAutomation()).
void reconcileTempoMap (te::Edit&);

// The tracks (model/arrangement/internal/Tracks.h): Tracktion gets a track for each of the document's, with its ID, in
// its place and folder (made as Tracktion makes one, with its fader and meter, or a plain folder's), and the mute, solo
// and solo isolation (a return's) that it plays with, and its write mode (the master's too); Tracktion's tracks that the
// document hasn't go, with what they hold. Nothing for a song without a track list (an Edit that isn't a song's).
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
// that the song hasn't are emptied. Not those of devices the song keeps curves for while an undo can bring them back, and
// not a curve Tracktion records, until it stops (adoptRecordedAutomation()).
void reconcileAutomation (te::Edit&);

// The same for one parameter's curve, after a change to it alone.
void reconcileCurve (te::Edit&, const ParameterRef&);

// Only the switches Tracktion keeps outside its undo history (the tracks' mute, solo and solo isolation), and the write
// modes, which Phazite sets outside it: after an undo or a redo (phazite::undo(), model/commands/internal/Undo.h), which
// puts the document's back but not those, when a step changed them (a bounce mutes the track it rendered); and after a
// write mode changed. Begins no undo step, so the redo history stays.
void reconcileTrackSwitches (te::Edit&);
} // namespace phazite::mirror
