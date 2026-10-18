// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace phazite
{
class CommandRegistry;
class ProjectContext;
class TrackRenderHost;

// The commands that work on the open song and need no window of their own, in the order the app registers them, which
// decides which of two commands on the same key runs, the order of the palette's equal matches and of the shortcuts list:
// undo, redo and the transport, then the selection's, the tracks' (with the playlist's track heights), the sections', the
// song outlines', the mixer's (with freezing and bouncing, whose renders the host shows) and the loop's. The project's own
// commands are ProjectSession's; the panels' are the UI's.
void addSongCommands (CommandRegistry&, ProjectContext&, TrackRenderHost&);
} // namespace phazite
