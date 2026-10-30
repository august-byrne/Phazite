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
} // namespace phazite::mirror
