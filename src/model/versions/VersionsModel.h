// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project's versions, as the Versions view, the activity bar, the status bar and the commands reach them
// (docs/versions.md): by plain values, versions and branches by their ids. What changes is told by the project context's
// listeners (ProjectContext::Listener::versionsChanged()).
#pragma once

#include "model/base/IDs.h"
#include "model/project/ProjectContext.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

struct VersionInfo
{
    juce::String id;
    juce::String name; // the one it was given, or the summary of its changes
    juce::String note;
    bool hasOwnName = false;
    juce::Time saved;
    juce::String branch;       // its branch's id
    juce::StringArray parents; // the versions it follows: none for the first, two for a merge
};

struct BranchInfo
{
    juce::String id, name;
    juce::Colour colour;
    juce::String head; // its last version's id; none before its first
};

// A line of the changes since the last version: a track (a folder or a return too), the master, or the song's own.
struct VersionChange
{
    enum class Kind
    {
        added,
        removed,
        changed
    };

    Kind kind = Kind::changed;
    TrackID track; // the track's, or the master's; none for the song's own
    bool isMaster = false, isSong = false;
    juce::String name; // "Bass", "Master", "Song"
    juce::String what; // "volume, 12 notes"; empty for a track added or taken away
};

// Whether the open project keeps versions (it has a project file), and whether they are still being read as it opens.
bool keepsVersions (const ProjectContext&);
bool isReadingVersions (const ProjectContext&);

// Every branch's versions, newest first; the branches, in the order they were made; the current one, which the project
// file is (none before the first version); and its last version.
std::vector<VersionInfo> getVersions (const ProjectContext&);
std::vector<BranchInfo> getBranches (const ProjectContext&);
std::optional<BranchInfo> getCurrentBranch (const ProjectContext&);
std::optional<VersionInfo> getLastVersion (const ProjectContext&);

// What the song holds now that the current branch's last version didn't, by track: none before the first version.
std::vector<VersionChange> getChangesSinceLastVersion (const ProjectContext&);
bool hasChangesSinceLastVersion (const ProjectContext&);

// The name and the note for the next version, as the Versions view holds them while they are typed.
void setVersionDraft (ProjectContext&, const juce::String& name, const juce::String& note);
juce::String getVersionDraftName (const ProjectContext&);
juce::String getVersionDraftNote (const ProjectContext&);

// Records a version of the song as it is now, with the draft's name and note (which it then clears), written in the
// background, and calls back once it is written, or not (why, then); false if it can't start (while one is being saved,
// while the versions are being read, or for a song that keeps none). Not an undo step: it changes the history, not the
// song.
bool saveVersion (ProjectContext&, std::function<void (juce::Result)> done = {});
bool isSavingVersion (const ProjectContext&);

// How many bytes the project's versions take on disk.
juce::int64 getVersionsSize (const ProjectContext&);

// Save Version (version.save): the project saved first, then a version recorded, as saveProjectThen() does it: the app's
// save, which asks where a project goes the first time, and calls back with whether it saved.
using SaveProjectThen = std::function<void (std::function<void (bool saved)>)>;
void saveProjectAndVersion (ProjectContext&, const SaveProjectThen&);
bool canSaveVersion (const ProjectContext&);
void addVersionCommands (CommandRegistry&, ProjectContext&, SaveProjectThen);
} // namespace phazite
