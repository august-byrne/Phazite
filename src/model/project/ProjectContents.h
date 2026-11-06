// SPDX-License-Identifier: AGPL-3.0-or-later
// What the open project holds, as the Project view in the left panel shows it (docs/design.md, section 4: the Project view):
// its notes; the audio files its song plays, its audio clips' and its devices' sound files, each found or missing; and the
// plug-ins it holds, each playing or what is wrong with it. By plain values and IDs; each function gives nothing, or does
// nothing, while no project is open. The audio files in its folder that nothing plays are Clean Up Unused Audio's
// (UnusedAudio.h), and a crashed plug-in is reloaded by reloadDevice() (model/mixer/MixerModel.h).
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace phazite
{
class ProjectContext;

// The notes, which the song keeps (Schema.h: notes), lines and all. Changed, they are a change of their own, "Change
// Notes", unless a gesture is open (model/commands/SongGesture.h), as while they are typed, whose one step they join.
juce::String getProjectNotes (const ProjectContext&);
void setProjectNotes (ProjectContext&, const juce::String&);

//==============================================================================
// An audio file the song plays: the path the song keeps for it (within the project's folder, "Audio/Kick.wav", or a full
// one), the file that names, whether it is missing, the channels that play it (their names, each once: a clip's track, or a
// device's track or the master), and how many clips and devices' sounds play it.
struct ProjectAudioFile
{
    juce::String path {};
    juce::File file {};
    bool missing = false;
    juce::StringArray channels {};
    int uses = 0;

    bool operator== (const ProjectAudioFile&) const = default;
};

// Each once, in the order of the channels that play it first, top to bottom, the master last.
std::vector<ProjectAudioFile> getProjectAudioFiles (const ProjectContext&);

// A missing file found again, or another put in its place: the project's copy of the file found (copied into its Audio
// folder, as an import copies it) takes the path's place everywhere the song plays it, as one undo step, "Locate Audio File".
// Whether anything changed: nothing for a path the song doesn't play, a file that isn't there, or one at the same path.
bool locateAudioFile (ProjectContext&, const juce::String& path, const juce::File& found);

//==============================================================================
// How a plug-in plays now (model/plugins/internal/PluginInstances.h).
enum class PluginState
{
    playing,
    crashed, // its helper went, or it crashed as it loaded: it plays nothing until it is reloaded (reloadDevice())
    missing, // it isn't installed here, or scanning hasn't found it
    off      // the project was opened with its plug-ins off, after a crash of Phazite's
};

// A plug-in the song holds: its device, the channel it is on (a track, or the master: its ID) with that channel's name, the
// plug-in as the song names it (its name, maker and format), whether it is an instrument, and how it plays.
struct ProjectPlugin
{
    DeviceID device {};
    TrackID channel {};
    juce::String channelName {}, name {}, maker {}, format {};
    bool isInstrument = false;
    PluginState state = PluginState::playing;

    bool operator== (const ProjectPlugin&) const = default;
};

// In the order of their channels, top to bottom, the master last, and on each in the order its sound goes through them,
// those in containers too.
std::vector<ProjectPlugin> getProjectPlugins (const ProjectContext&);
} // namespace phazite
