// SPDX-License-Identifier: AGPL-3.0-or-later
// Songs that show what Phazite does (docs/design.md, section 13; the owner, 2026-11-01): Help > Example Songs and the
// welcome window's last page open one as a new untitled project, to play, take apart and save as one's own, while a new
// project starts empty. Each is made in code through the model's functions by ID, as the UI makes a song, so that it keeps
// working however the model holds the song (Bitwig's, Studio One's and Reason's demo songs are files of their own, which
// Phazite would have to keep up with every change of its format while it is pre-alpha). Their sounds are Phazite's devices
// with Phazite's own presets, and samples each synthesizes as it is made (Phazite ships no sound files).
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace phazite
{
class ProjectContext;

struct ExampleSong
{
    const char* id;          // its command is "help.example." and this
    const char* name;        // the untitled project's, until it is saved
    const char* description; // what it shows, for menus, the command search and the welcome window
    const char* playedTrack; // the track selected as it opens, which the computer keyboard then plays
};

// In the order menus list them: the simplest first.
const std::vector<ExampleSong>& getExampleSongs();
const ExampleSong* findExampleSong (const juce::String& id);

// Makes an example song in the open project, which should be empty: the app makes it in a new project before its first
// save, so that it opens without undo steps or unsaved changes. Its devices load Phazite's presets from that folder (the
// "presets" folder next to the app, resources/presets in the source tree), each at its defaults where a preset is missing.
// Returns false for an id that isn't one.
bool addExampleSong (ProjectContext&, const juce::String& id, const juce::File& factoryPresets);

// The track an example song plays first from the computer keyboard (its playedTrack), in the open project; none if it
// has no track of that name.
TrackID findExampleTrackToPlay (const ProjectContext&, const ExampleSong&);
} // namespace phazite
