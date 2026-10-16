// SPDX-License-Identifier: AGPL-3.0-or-later
// Folders as buses (docs/design.md, section 8). A bus mixes the tracks in it together, through its own devices, fader
// and meter, as group tracks do in Ableton Live and Bitwig and folders in REAPER; every new folder is one (design
// question 11). A plain folder, which its bus switch makes, only scales its tracks' volumes, as a VCA does. Below the
// rest of the mixer (Mixer.h), so that the folders' code (arrangement/Tracks.h) can make buses.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
bool isBus (const te::Track&);
void makeBus (te::FolderTrack&);         // its fader starts where the folder's volume was
void makePlainFolder (te::FolderTrack&); // its volume starts where the bus's fader was; takes its devices away

// Makes a folder a bus with its switch on, so that it stays one without effects (setMixedAsBus() in Mixer.h): what every
// new folder is. Joins the caller's undo step.
void mixAsBus (te::FolderTrack&);
} // namespace phazite
