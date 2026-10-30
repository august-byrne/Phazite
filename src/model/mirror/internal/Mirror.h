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

namespace phazite::mirror
{
// The tempo map (model/transport/internal/Tempo.h): Tracktion's tempo sequence gets the document's tempos and time
// signatures, each at its beat.
void reconcileTempoMap (te::Edit&);

// The tracks (model/arrangement/internal/Tracks.h): Tracktion gets a track for each of the document's, with its ID, in
// its place and folder (made as Tracktion makes one, with its fader and meter, or a plain folder's), and the mute, solo
// and solo isolation (a return's) that it plays with; Tracktion's tracks that the document hasn't go, with what they hold.
// Nothing for a song without a track list (an Edit that isn't a song's).
void reconcileTracks (te::Edit&);

// Only the switches Tracktion keeps outside its undo history (the tracks' mute, solo and solo isolation): after an undo or
// a redo (phazite::undo(), model/commands/internal/Undo.h), which puts the document's back but not those, when a step changed
// them (a bounce mutes the track it rendered). Begins no undo step, so the redo history stays.
void reconcileTrackSwitches (te::Edit&);
} // namespace phazite::mirror
