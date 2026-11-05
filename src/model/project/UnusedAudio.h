// SPDX-License-Identifier: AGPL-3.0-or-later
// Clean Up Unused Audio... (docs/versions.md: outside the steps): the audio files in the open project's Audio and Generated
// folders that neither its song plays, nor any of its versions, nor a branch's song as it was left, to delete those the user
// ticks. Takes, imports and generated sounds pile up there, versions or not. Deleting moves them to the recycle bin, where
// they can still be found: no undo brings them back, and a clip that an undo brings back plays without them. Not an undo
// step.
#pragma once

#include "model/project/ProjectContext.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace phazite
{
struct UnusedAudioFile
{
    juce::File file;
    juce::String name; // its place in the project's folder: "Audio/Take 3.wav"
    juce::int64 size = 0;
};

// Whether it can look now: a project is open, it doesn't record (a take being written), and its versions are read.
bool canCleanUpAudio (const ProjectContext&);

// Those files, by their names; none while it can't look.
std::vector<UnusedAudioFile> findUnusedAudio (const ProjectContext&);

// Moves those of the files given that are still unused to the recycle bin (or deletes them outright: the tests'), and says how
// many went.
enum class Removal
{
    toRecycleBin,
    outright
};

int deleteUnusedAudio (ProjectContext&, const juce::Array<juce::File>&, Removal = Removal::toRecycleBin);
} // namespace phazite
